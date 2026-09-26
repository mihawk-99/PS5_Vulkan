/*
 * PS5 Vulkan driver - Phase D2: compute pipelines and dispatch.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase D2 (docs/M5_REFERENCE.md). The console's compute queue is
 * proven at the raw-packet level (V0-compute, `c0-dispatch`, golden/c0): a
 * compiled ISA, a descriptor table holding one storage buffer, the COMPUTE_*
 * SH registers, PM4 DISPATCH_DIRECT and a CS partial flush all ran and read
 * their word back. This file is that same shape behind the Vulkan API --
 * vkCreateComputePipelines compiles the application's SPIR-V and keeps the ISA
 * in a GPU-visible mapping, and vkCmdDispatch records the packets into the
 * command buffer's stream, which the queue submits like any draw.
 *
 * A dispatch uses the draw path's descriptor writer: one table per set and
 * every declared binding at its compiler-reported offset, with the table
 * pointer in the compiler-reported user-data dword.
 */

#include "ps5vk_private.h"

#include <assert.h>
#include <string.h>

#include "util/macros.h"

/* The SH register block a dispatch programs, in the AGC byte offsets
 * src/diagnostics.cpp names; the packet carries (offset - 0xb000) / 4 as the
 * register index. */
#define PS5VK_COMPUTE_REG_START 0xb810           /* COMPUTE_START_X/Y/Z */
#define PS5VK_COMPUTE_REG_THREADS 0xb81c         /* COMPUTE_NUM_THREAD_X/Y/Z */
#define PS5VK_COMPUTE_REG_PROGRAM 0xb830         /* COMPUTE_PGM_LO/HI */
#define PS5VK_COMPUTE_REG_RESOURCES 0xb848       /* COMPUTE_PGM_RSRC1/2 */
/* The compiler reports shader registers as dword offsets from SI_SH_REG_OFFSET
 * (0xb000), not as the raw addresses the dispatch programs: PGM_LO is 0x20c,
 * RSRC1 0x212, RSRC2 0x213, RSRC3 0x228, NUM_THREAD_X 0x207. 0.3.0's AGC package
 * writer reads the same table (src/platform/ps5_agc_package.c,
 * compute_metadata_valid), so both consumers agree by construction. */
#define PS5VK_COMPUTE_META_RSRC1 0x212           /* COMPUTE_PGM_RSRC1 */
#define PS5VK_COMPUTE_META_RSRC2 0x213           /* COMPUTE_PGM_RSRC2 */
#define PS5VK_COMPUTE_META_RSRC3 0x228           /* COMPUTE_PGM_RSRC3 */
#define PS5VK_COMPUTE_REG_LIMITS 0xb854          /* COMPUTE_RESOURCE_LIMITS */
#define PS5VK_COMPUTE_REG_RESOURCE3 0xb8a0       /* COMPUTE_PGM_RSRC3 */
#define PS5VK_COMPUTE_REG_USER_DATA 0xb900       /* COMPUTE_USER_DATA_0.. */
#define PS5VK_COMPUTE_REG_DESTINATIONS 0xb858    /* COMPUTE_DESTINATION_EN_SE0/1 */
#define PS5VK_COMPUTE_REG_NO_DESTINATIONS 0xb864 /* ..._SE2/3 */
#define PS5VK_COMPUTE_REG_ACCUMULATORS 0xb890    /* COMPUTE_USER_ACCUM_0..3 */

/* PM4 DISPATCH_DIRECT (0x15) and EVENT_WRITE (0x46). The initiator combines
 * COMPUTE_SHADER_EN, ORDER_MODE and CS_W32_EN; the wave-size bit has to match
 * the wave size the compiler allocated for. */
#define PS5VK_DISPATCH_DIRECT_OPCODE 0x15u
#define PS5VK_EVENT_WRITE_OPCODE 0x46u
#define PS5VK_DISPATCH_SHADER_ENABLE 0x0001u
#define PS5VK_DISPATCH_ORDER_MODE 0x0040u
#define PS5VK_DISPATCH_WAVE32 0x8000u
/* Mesa's ac_cmdbuf.h writes event 7, CS_PARTIAL_FLUSH, before a cache or EOP
 * operation that has to observe the compute waves. */
#define PS5VK_EVENT_CS_PARTIAL_FLUSH 0x00000407u

/* The registers, the DISPATCH_DIRECT packet and the flush, with room to spare. */
#define PS5VK_COMPUTE_MAX_WORDS 96

/* One direct SET_SH_REG packet: the header with its value count, the register
 * index in the 0xb000-relative space, then the values. */
static uint32_t *ps5vk_compute_sh_registers(uint32_t *at, uint32_t registers,
                                 const uint32_t *values, uint32_t count)
{
   at[0] = 0xc0007600u | (count << 16);
   at[1] = (registers - 0xb000u) / 4u;
   memcpy(at + 2, values, count * sizeof(*values));
   return at + 2 + count;
}

/* The shader's local size, from its SPIR-V: OpExecutionMode <id> LocalSize x y
 * z. A dispatch programs it as COMPUTE_NUM_THREAD_X/Y/Z, which the hardware
 * takes as the workgroup's shape -- the console's probe programmed 1, 1, 1 for
 * the same shader, and a dispatch that leaves it zero launches no threads. */
static bool ps5vk_spirv_local_size(const struct ps5vk_shader_module *module, uint32_t size[3])
{
   const size_t count = module->size / sizeof(uint32_t);
   if (count < PS5VK_SPIRV_HEADER_WORDS || module->words[0] != PS5VK_SPIRV_MAGIC)
      return false;
   bool found = false;
   for (size_t at = PS5VK_SPIRV_HEADER_WORDS; at < count;)
   {
      const uint32_t word_count = module->words[at] >> 16;
      const uint32_t opcode = module->words[at] & 0xffff;
      if (word_count == 0 || word_count > count - at)
         return false;
      if (opcode == PS5VK_SPIRV_OP_EXECUTION_MODE && word_count >= 6 &&
         module->words[at + 2] == PS5VK_SPIRV_EXECUTION_MODE_LOCAL_SIZE)
      {
         for (unsigned axis = 0; axis < 3; axis++)
            size[axis] = module->words[at + 3 + axis];
         found = true;
      }
      at += word_count;
   }
   return found;
}

/* The ISA of a shader module, compiled for the console, with the words the
 * compiler reported. The console's libpsbc reports them in the shader metadata
 * (tooling/psbc/patch-compute-metadata.py); a build whose archive cannot is a
 * failure to see, not a reason to guess. */
static VkResult ps5vk_compute_pipeline_compile(struct ps5vk_device *device,
                                               const VkComputePipelineCreateInfo *info,
                                               struct ps5vk_pipeline *pipeline)
{
   VK_FROM_HANDLE(ps5vk_shader_module, module, info->stage.module);
   VK_FROM_HANDLE(vk_pipeline_layout, layout, info->layout);
   if (module == NULL)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                         "a compute stage without a shader module is not supported");
   if (!ps5vk_spirv_has_entry_point(module, PS5VK_SPIRV_EXECUTION_MODEL_COMPUTE,
                                     info->stage.pName))
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                         "compute stage: not well-formed SPIR-V with an entry point \"%s\"",
                         info->stage.pName);

   PsbcCompileOptions options = {
      .target = PSBC_TARGET_PS5,
      .stage = PSBC_STAGE_COMPUTE,
      .entrypoint = info->stage.pName,
      .optimise = true,
      .address32_hi = (uint32_t)PS5VK_ADDRESS_HIGH_WORD,
   };
   VkResult result =
      ps5vk_descriptor_options(device, layout, VK_SHADER_STAGE_COMPUTE_BIT, &options);
   /* R9: the stage's specialization constants, as a graphics stage's
    * (ps5vk_specialization_options). A workgroup size a constant sets is not
    * what the module's LocalSize says, and the check below refuses that by name. */
   if (result == VK_SUCCESS)
      result = ps5vk_specialization_options(device, info->stage.pSpecializationInfo, "compute",
                                            &options);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; layout && i < layout->push_range_count; i++) {
      const VkPushConstantRange *range = &layout->push_ranges[i];
      pipeline->push_constant_bytes = MAX2(pipeline->push_constant_bytes, range->offset + range->size);
      pipeline->push_constant_stages |= range->stageFlags;
   }
   if (pipeline->push_constant_bytes > PS5VK_MAX_PUSH_CONSTANT_BYTES)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "compute push constants exceed the driver budget");

   /* R10: what this compiler has no path for is refused before it runs, so the
    * application gets a result and a sentence instead of a dead process. */
   {
      char reason[PS5VK_CAPABILITY_LIST_BYTES];
      if (ps5vk_spirv_refusal((const uint32_t *)module->words, module->size, reason,
                              sizeof(reason)))
         return vk_errorf(device, VK_ERROR_UNKNOWN, "the compute shader: %s (docs/M5_PHASE_C.md, "
                                                   "R10)",
                          reason);
   }

   call_once(&ps5vk_compile_once, ps5vk_compile_mutex_init);
   mtx_lock(&ps5vk_compile_mutex);
   PsbcShaderOutput output;
   memset(&output, 0, sizeof(output));
   bool aborted = false;
   const PsbcResult compiled = ps5vk_compile_shader_deep(
      NULL, (const uint32_t *)module->words, module->size, &options, &output, &aborted);
   mtx_unlock(&ps5vk_compile_mutex);
   const PsbcShaderMetadata metadata = output.metadata;
   const bool produced =
      compiled == PSBC_RESULT_OK && output.machine_code != NULL && output.machine_code_size != 0;
   if (!produced)
   {
      psbc_free_output(&output);
      if (aborted) {
         char declared[PS5VK_CAPABILITY_LIST_BYTES];
         ps5vk_spirv_capability_list((const uint32_t *)module->words, module->size, declared,
                                     sizeof(declared));
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "the compute shader compiler aborted on this shader instead of returning "
                          "a result, so no package was written: it cannot lower something the "
                          "shader uses (the shader declares %s; docs/M5_PHASE_C.md, R10)",
                          declared);
      }
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                         "the compute shader did not compile: %s (result %d)",
                         psbc_result_string(compiled), (int)compiled);
   }
   /* The 0.3.0 fork reports the dispatch's resource words in the shader register
    * table -- R_00B848_COMPUTE_PGM_RSRC1, R_00B84C_COMPUTE_PGM_RSRC2 and
    * R_00B8A0_COMPUTE_PGM_RSRC3, the offsets the dispatch programs -- where the
    * 0.2.0-era compiler exported them as named metadata fields. Either way they
    * are the compiler's own statement about this shader, so the dispatch programs
    * exactly what it wrote (docs/BLOCKERS.md, the SDK fork migration). */
   uint32_t rsrc1 = 0;
   uint32_t rsrc2 = 0;
   uint32_t rsrc3 = 0;
   bool have_rsrc1 = false;
   bool have_rsrc2 = false;
   bool have_rsrc3 = false;
   for (uint32_t i = 0; i < metadata.shader_register_count; ++i)
   {
      const PsbcRegisterWrite *const write = &metadata.shader_registers[i];
      switch (write->offset)
      {
      case PS5VK_COMPUTE_META_RSRC1:
         rsrc1 = write->value;
         have_rsrc1 = true;
         break;
      case PS5VK_COMPUTE_META_RSRC2:
         rsrc2 = write->value;
         have_rsrc2 = true;
         break;
      case PS5VK_COMPUTE_META_RSRC3:
         rsrc3 = write->value;
         have_rsrc3 = true;
         break;
      default:
         break;
      }
   }
   if (!have_rsrc1 || !have_rsrc2 || !have_rsrc3)
   {
      psbc_free_output(&output);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                         "the compiler reported no COMPUTE_PGM_RSRC words for the dispatch");
   }
   if (metadata.user_sgpr_count > PS5VK_MAX_USER_DATA ||
       metadata.descriptor_binding_count > PSBC_MAX_DESCRIPTOR_BINDINGS)
   {
      psbc_free_output(&output);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                         "compute resource metadata exceeds %u user-data dwords or %u bindings",
                         PS5VK_MAX_USER_DATA, PSBC_MAX_DESCRIPTOR_BINDINGS);
   }

   /* A direct mapping is a whole number of direct-memory pages: the ISA is
     * padded up to one, and the dispatch points at its start. */
   const size_t code_bytes = output.machine_code_size;
   const size_t mapping_bytes = (size_t)ALIGN_POT(code_bytes, PS5VK_DIRECT_PAGE_BYTES);
   const int32_t mapped = ps5vk_direct_mapping_create(&pipeline->compute.code, mapping_bytes,
                                                       PS5VK_DIRECT_PAGE_BYTES, PS5VK_DIRECT_COMPUTE);
   if (mapped != 0)
   {
      psbc_free_output(&output);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                         "the shader code could not be mapped in the address window: 0x%08x",
                         (unsigned)mapped);
   }
   if ((uint64_t)(uintptr_t)pipeline->compute.code.address >> 32 != PS5VK_ADDRESS_HIGH_WORD)
   {
      ps5vk_direct_mapping_destroy(&pipeline->compute.code);
      psbc_free_output(&output);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                         "the shader code lies outside the compiled 4 GiB window");
   }
   memset(pipeline->compute.code.address, 0, pipeline->compute.code.bytes);
   memcpy(pipeline->compute.code.address, output.machine_code, code_bytes);
   ps5vk_flush_cpu_cache(pipeline->compute.code.address, code_bytes);

   /* The fork reports the wave size ACO compiled for, so DISPATCH_DIRECT's
     * CS_W32_EN is the compiler's own answer instead of an inference from RSRC1's
     * VGPR granule: the 0.2.0-era compiler reported the granule and the VGPR count
     * and left that step here. */
   const uint32_t wave_size = metadata.compute_wave_size;
   if (wave_size != 32 && wave_size != 64)
   {
      ps5vk_direct_mapping_destroy(&pipeline->compute.code);
      psbc_free_output(&output);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                         "the compiler reported wave size %u, which is neither 32 nor 64",
                         (unsigned)wave_size);
   }
   const bool wave32 = wave_size == 32;

   uint32_t local_size[3] = {0, 0, 0};
   if (!ps5vk_spirv_local_size(module, local_size) || local_size[0] == 0 || local_size[1] == 0 ||
      local_size[2] == 0)
   {
      ps5vk_direct_mapping_destroy(&pipeline->compute.code);
      psbc_free_output(&output);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                         "the compute shader declares no local size, which a dispatch programs");
   }

   /* The compiler reports the workgroup shape it compiled for; the module's own
    * local size is what the dispatch programs. Two statements about one dispatch,
    * and the agreement the granule-and-VGPR pair used to establish. */
   if (metadata.compute_workgroup_size[0] != local_size[0] ||
      metadata.compute_workgroup_size[1] != local_size[1] ||
      metadata.compute_workgroup_size[2] != local_size[2])
   {
      ps5vk_direct_mapping_destroy(&pipeline->compute.code);
      psbc_free_output(&output);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                         "the compiler's workgroup shape %ux%ux%u differs from the module's local "
                         "size %ux%ux%u",
                         (unsigned)metadata.compute_workgroup_size[0],
                         (unsigned)metadata.compute_workgroup_size[1],
                         (unsigned)metadata.compute_workgroup_size[2], (unsigned)local_size[0],
                         (unsigned)local_size[1], (unsigned)local_size[2]);
   }

   pipeline->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
   pipeline->compute.metadata = metadata;
   memcpy(pipeline->compute.local_size, local_size, sizeof(local_size));
   /* The runner's capture logs the mapping this dispatch points at, exactly as
     * it logs a graphics pipeline's stage workspace. */
   pipeline->next_stage = device->stages;
   device->stages = pipeline;
   pipeline->stage_registered = true;
   pipeline->compute.code_bytes = (uint32_t)code_bytes;
   pipeline->compute.rsrc1 = rsrc1;
   pipeline->compute.rsrc2 = rsrc2;
   pipeline->compute.rsrc3 = rsrc3;
   pipeline->compute.wave32 = wave32;
   psbc_free_output(&output);
   return VK_SUCCESS;
}

static VkResult ps5vk_compute_pipeline_create(struct ps5vk_device *device,
                                              const VkComputePipelineCreateInfo *info,
                                              const VkAllocationCallbacks *allocator,
                                              VkPipeline *out_pipeline)
{
   struct ps5vk_pipeline *const pipeline =
      vk_object_zalloc(&device->vk, allocator, sizeof(*pipeline), VK_OBJECT_TYPE_PIPELINE);
   if (!pipeline)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   /* A compute pipeline's shader code is its own mapping, not the graphics
     * stage workspace, so the stage mapping stays absent (start -1) and the
     * dispatch reads only the compiled ISA. */
   pipeline->shaders.stage.start = -1;
   mtx_init(&pipeline->shaders.lock, mtx_plain);
   const VkResult result = ps5vk_compute_pipeline_compile(device, info, pipeline);
   if (result != VK_SUCCESS)
   {
      ps5vk_pipeline_free(device, pipeline, allocator);
      return result;
   }
   *out_pipeline = ps5vk_pipeline_to_handle(pipeline);
   return VK_SUCCESS;
}

static VkResult
ps5vk_CreateComputePipelines_untimed(
   VkDevice _device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
   const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator,
   VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* There is no pipeline cache: every pipeline compiles. */
   (void)pipelineCache;
   VkResult result = VK_SUCCESS;
   uint32_t index = 0;
   for (; index < createInfoCount; index++)
   {
      pPipelines[index] = VK_NULL_HANDLE;
      const VkResult created = ps5vk_compute_pipeline_create(device, &pCreateInfos[index],
                                                               pAllocator, &pPipelines[index]);
      if (created == VK_SUCCESS)
         continue;
      if (result == VK_SUCCESS)
         result = created;
      if (pCreateInfos[index].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)
         break;
   }
   for (; index < createInfoCount; index++)
      pPipelines[index] = VK_NULL_HANDLE;
   return result;
}

/* Timed for the hitch report (ps5vk_queue.c). */
VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateComputePipelines(
   VkDevice _device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
   const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator,
   VkPipeline *pPipelines)
{
   const uint64_t hitch = ps5vk_hitch_begin();
   const VkResult result = ps5vk_CreateComputePipelines_untimed(_device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
   ps5vk_hitch_end(PS5VK_HITCH_PIPELINE, hitch);
   return result;
}

/* One dispatch's workgroups, whichever command named them: a direct dispatch
 * passes its arguments, an indirect one the three dwords it read from the
 * buffer. The shared resource writer builds the stage's tables, then the
 * packets the V0-compute probe proved -- the COMPUTE_* SH registers,
 * DISPATCH_DIRECT and a CS partial flush -- go into the command buffer's
 * stream, which the queue submits and waits on like any other. */
static void
ps5vk_dispatch(struct ps5vk_cmd_buffer *cmd_buffer, uint32_t groupCountX, uint32_t groupCountY,
               uint32_t groupCountZ)
{
   struct ps5vk_pipeline *const pipeline = cmd_buffer->compute_pipeline;
   if (pipeline == NULL || pipeline->bind_point != VK_PIPELINE_BIND_POINT_COMPUTE)
   {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                        "a dispatch with no compute pipeline bound; vkCmdBindPipeline with "
                        "VK_PIPELINE_BIND_POINT_COMPUTE names one (Phase D2)");
      return;
   }
   /* A dispatch of no workgroups does nothing, which Vulkan allows. */
   if (groupCountX == 0 || groupCountY == 0 || groupCountZ == 0)
      return;
   const PsbcShaderMetadata *const metadata = &pipeline->compute.metadata;
   const VkShaderStageFlags stage_bit = VK_SHADER_STAGE_COMPUTE_BIT;
   uint32_t user_data[1][PS5VK_MAX_USER_DATA] = {{0}};
   bool colour_barrier = false;
   if (!ps5vk_cmd_buffer_shader_resources(cmd_buffer, pipeline, &metadata, &stage_bit, 1,
                                          user_data, &colour_barrier))
      return;
   /* The GPU barrier completes and flushes preceding colour and depth writes
    * before compute samples them, just as the draw path does (R70). */
   if (colour_barrier && !ps5vk_cmd_buffer_gpu_barrier(cmd_buffer))
      return;

   const uint64_t code_address = (uint64_t)(uintptr_t)pipeline->compute.code.address;
   const uint32_t start[3] = {0, 0, 0};
   const uint32_t threads[3] = {pipeline->compute.local_size[0], pipeline->compute.local_size[1],
                                 pipeline->compute.local_size[2]};
   const uint32_t program[2] = {(uint32_t)(code_address >> 8), (uint32_t)(code_address >> 40)};
   const uint32_t resources[2] = {pipeline->compute.rsrc1, pipeline->compute.rsrc2};
   const uint32_t limits[1] = {0};
   const uint32_t resource3[1] = {pipeline->compute.rsrc3};
   const uint32_t destinations[2] = {0xffffffffu, 0xffffffffu};
   const uint32_t no_destinations[2] = {0, 0};
   const uint32_t accumulators[4] = {0, 0, 0, 0};
   const uint32_t dispatch[4] = {groupCountX, groupCountY, groupCountZ,
                                  PS5VK_DISPATCH_SHADER_ENABLE | PS5VK_DISPATCH_ORDER_MODE |
                                      (pipeline->compute.wave32 ? PS5VK_DISPATCH_WAVE32 : 0u)};

   uint32_t words[PS5VK_COMPUTE_MAX_WORDS];
   uint32_t *at = words;
   at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_START, start, 3);
   at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_THREADS, threads, 3);
   at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_PROGRAM, program, 2);
   at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_RESOURCES, resources, 2);
   at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_LIMITS, limits, 1);
   at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_RESOURCE3, resource3, 1);
   if (metadata->user_sgpr_count != 0)
      at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_USER_DATA, user_data[0],
                                     metadata->user_sgpr_count);
   at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_DESTINATIONS, destinations, 2);
   at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_NO_DESTINATIONS, no_destinations, 2);
   at = ps5vk_compute_sh_registers(at, PS5VK_COMPUTE_REG_ACCUMULATORS, accumulators, 4);
   at[0] = 0xc0000000u | (3u << 16) | (PS5VK_DISPATCH_DIRECT_OPCODE << 8);
   memcpy(at + 1, dispatch, sizeof(dispatch));
   at += 5;
   at[0] = 0xc0000000u | (PS5VK_EVENT_WRITE_OPCODE << 8);
   at[1] = PS5VK_EVENT_CS_PARTIAL_FLUSH;
   at += 2;
   const uint32_t word_count = (uint32_t)(at - words);
   uint32_t *const recorded = util_dynarray_grow(&cmd_buffer->words, uint32_t, word_count);
   if (recorded == NULL)
   {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY, "no memory for a dispatch");
      return;
   }
   memcpy(recorded, words, word_count * sizeof(*words));
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY,
                  uint32_t groupCountZ)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   ps5vk_dispatch(cmd_buffer, groupCountX, groupCountY, groupCountZ);
}

/* R84: vkCmdDispatchBase (Vulkan 1.1). A zero base is vkCmdDispatch. A
 * non-zero one needs the compiler to take the base as the workgroup ID's
 * offset, which it does not yet, so it is refused by name rather than dispatched
 * from the wrong workgroups (docs/M5_PHASE_C.md, R84). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t baseGroupX, uint32_t baseGroupY,
                      uint32_t baseGroupZ, uint32_t groupCountX, uint32_t groupCountY,
                      uint32_t groupCountZ)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   if ((baseGroupX | baseGroupY | baseGroupZ) != 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "a dispatch based at workgroup (%u, %u, %u): the compiler does not "
                              "take a base workgroup yet (R84)",
                              baseGroupX, baseGroupY, baseGroupZ);
      return;
   }
   ps5vk_dispatch(cmd_buffer, groupCountX, groupCountY, groupCountZ);
}

/* vkCmdDispatchIndirect: the same dispatch with its three workgroup counts read
 * from the bound buffer, as vkCmdDrawIndirect reads its parameters
 * (ps5vk_draw.c). A buffer is host memory here, so the read is a memcpy; the
 * structure is Vulkan's own VkDispatchIndirectCommand, three four-byte counts.
 * The parameters are read when the dispatch is recorded, so a command buffer
 * that writes them itself cannot be the one that dispatches (the same rule
 * vkCmdDrawIndirect follows). */
VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_buffer, buffer, _buffer);
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   const uint32_t command_bytes = 3u * sizeof(uint32_t);
   assert(buffer != NULL && (offset % 4) == 0);
   if (buffer->vk.device_address == 0) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "an indirect dispatch names a buffer with no GPU address: it has to "
                              "be bound to memory");
      return;
   }
   assert(offset + command_bytes <= buffer->vk.size);
   const uint64_t address = buffer->vk.device_address + offset;
   if (ps5vk_cmd_buffer_writes_range(cmd_buffer, address, command_bytes)) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_UNKNOWN,
                              "an indirect dispatch whose parameters this command buffer writes; "
                              "the parameters are read when the dispatch is recorded, and a "
                              "split-point read is a later step (docs/M5_REFERENCE.md)");
      return;
   }
   ps5vk_cmd_buffer_wait_submitted(cmd_buffer);
   ps5vk_flush_cpu_cache((const void *)(uintptr_t)address, command_bytes);
   uint32_t counts[3] = {0, 0, 0};
   memcpy(counts, (const void *)(uintptr_t)address, command_bytes);
   ps5vk_dispatch(cmd_buffer, counts[0], counts[1], counts[2]);
}
