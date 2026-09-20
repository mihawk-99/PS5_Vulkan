/*
 * PS5 Vulkan driver - the core commands the runtime leaves NULL.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A Vulkan 1.0 application that calls a core command this driver has no path
 * for asks for its entry point first, and the runtime's shared dispatch table
 * answers: with a trampoline or an implementation of its own for most commands,
 * and with a stub for the rest (Mesa's `vk_common_*` wrappers over empty slots,
 * merged by `vk_device_dispatch_table_from_entrypoints`, driver/ps5vk_device.c).
 * A stub is what the loader turns into an abort, so the application dies
 * without being told what is missing. The entry points below refuse by
 * name instead, the way every other unimplemented path in this driver does
 * (ps5vk_cmd_buffer_refuse, driver/ps5vk_private.h): the recording ends with
 * VK_ERROR_UNKNOWN and the message says which phase would implement it.
 *
 * Only the verified ones are here, and that distinction is a measurement rather
 * than a guess. Two calls were needed to make it: calling a command whose entry
 * point exists with an empty handle crashes too, which says nothing about the
 * command (the first reading of this round made exactly that mistake,
 * docs/M5_PHASE_B.md, 2026-09-18), so each command below was called again with
 * the handles a valid call needs, and each still crashed -- the runtime's
 * trampoline forwarding to an empty slot, which is a genuine gap. The commands
 * the runtime itself implements were left alone: one of them,
 * vkCmdBeginRenderPass, is a path this driver relies on, because Mesa's meta
 * operations clear through it.
 *
 * The list has been shrinking as phases land: vkGetDeviceMemoryCommitment,
 * vkCmdFillBuffer, vkCmdUpdateBuffer, vkCmdCopyImageToBuffer,
 * vkCmdClearColorImage, vkCmdDrawIndirect, vkCmdDrawIndexedIndirect,
 * vkCreateEvent, the pipeline cache family and vkCmdExecuteCommands are all
 * implemented now, each with the test that proves it named in its own note
 * below.
 *
 * Nothing here renders, copies or dispatches anything, so no stream changes:
 * these entry points exist only where a call would otherwise be a stub jump.
 */

#include "ps5vk_private.h"

/* Compute pipelines and dispatch were here and are implemented now
 * (ps5vk_CreateComputePipelines and ps5vk_CmdDispatch, driver/ps5vk_compute.c):
 * the compiled ISA in a GPU-visible mapping, one storage-buffer descriptor and
 * the packets the V0-compute probe proved on the console. */

/* The colour clear was here and is implemented now
 * (ps5vk_CmdClearColorImage, driver/ps5vk_image.c): a clear is the same CPU work
 * at a submission split point, writing one encoded texel through the image's own
 * map. The depth and stencil clear below is still refused. */


/* vkCmdClearDepthStencilImage was here and is implemented now
 * (ps5vk_CmdClearDepthStencilImage, driver/ps5vk_image.c): the same CPU clear at
 * a split point the colour clear is, through the depth image's own measured
 * map. */


/* The other transfers among these -- vkCmdFillBuffer and vkCmdUpdateBuffer --
 * are implemented now (driver/ps5vk_cmd_buffer.c, Phase C2): both are CPU work
 * at a submission split point, which is the mechanism the copies already use,
 * so neither needed a probe. Reading an image back into a buffer has no path
 * yet, which is what the refusal below says. */

/* Reading an image back into a buffer was here and is implemented now
 * (ps5vk_CmdCopyImageToBuffer2KHR, driver/ps5vk_image.c): it is the same CPU
 * work at a submission split point, over the image's own texel map. */

/* ---------------- objects and commands a second sweep found ----------------
 *
 * The sweep that found the six above was repeated for every 1.0 command whose
 * arguments this driver can make valid, one fork per call, and each crash was
 * then re-checked with the handles a valid call needs (docs/M5_PHASE_B.md,
 * 2026-09-18). What is left of that list is these: one object creation
 * (vkCreateBufferView) and three recording commands.
 * vkGetDeviceMemoryCommitment is not among them -- its entry point is missing
 * too, but the answer is the driver's own state, so it is implemented rather
 * than refused. */

/* Events are implemented now (ps5vk_sync.c, Phase B5): an event is the host
 * flag a binary sync object already was, vkSetEvent and vkResetEvent set and
 * clear it, and the three recording commands -- vkCmdSetEvent, vkCmdResetEvent
 * and vkCmdWaitEvents -- record a split with the action on it, which is the
 * mechanism the CPU-side transfers use and is why no probe was needed first. */

/* Pipeline caches are implemented now (ps5vk_pipeline_cache.c, Phase B6): AGC
 * still compiles each pipeline when it is created, so the cache is the empty
 * cache an implementation is allowed to have -- the object exists, its data is
 * zero bytes and a merge has nothing to do. */

/* Buffer views were here and are implemented now (ps5vk_CreateBufferView and
 * ps5vk_DestroyBufferView, driver/ps5vk_buffer.c): the object exists, and the
 * format a view may name is the reporting rule -- a format carrying the
 * texel-buffer feature the buffer's usage asks for. No format reports one yet
 * (the texel-buffer families are Phase D2's, docs/V0_FORMATS_AUDIT.md), so a
 * valid-shaped call is refused for that reason, which an application can see
 * for itself with vkGetPhysicalDeviceFormatProperties. */

/* Executing a secondary command buffer was here and is implemented now
 * (ps5vk_CmdExecuteCommands, driver/ps5vk_cmd_buffer.c, Phase B8): the
 * secondary's recorded words are copied into the primary's stream at the call,
 * which is what the queue submits, and the targets it renders into become the
 * submission's. The INDIRECT_BUFFER route B8 measured as faulting the GPU is
 * not used. */

/* vkCmdWriteTimestamp was here and is implemented now (ps5vk_CmdWriteTimestamp,
 * driver/ps5vk_query.c): V0-query's timestamp probe measured the console's GPU
 * clock (a RELEASE_MEM with the BOTTOM_OF_PIPE_TS event and an EOP selector
 * naming the timestamp, about 100 MHz, pid 143), so a timestamp pool is a type
 * this driver creates and the command records the clock into it. */

/* Reading a query pool's results with a copy into a buffer was here and is
 * implemented now (ps5vk_CmdCopyQueryPoolResults, driver/ps5vk_query.c): the
 * results are CPU work, recorded as a copy at a split point like a fill or an
 * image clear. */

/* The indirect draws were here and are implemented now (ps5vk_CmdDrawIndirect,
 * driver/ps5vk_draw.c): the parameters are read from the bound buffer when the
 * draw is recorded, which is what a CPU-side driver can do, and a command buffer
 * that writes that buffer itself refuses them by name (the split-point read is a
 * later step). Nothing else in this file changed with them.
 */
