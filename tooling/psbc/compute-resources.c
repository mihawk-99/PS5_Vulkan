/* PS5 Vulkan compatibility probe - record a compute dispatch's resource words.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A compute dispatch programs COMPUTE_PGM_RSRC1/2/3 itself, and on this
 * console the words come from the compiler's metadata. The PC runner links
 * ps5-opengl's own archive, which has no field for them, so the words are
 * recorded beside the payload once and read back there
 * (probes/c0/resources.txt, tools/build-compute-probe.sh).
 *
 * This prints the four values with the same options the runner's probe
 * compiles with, so the recorded words describe the same shader: PS5 target,
 * compute stage, optimised, and binding 0 declared as a storage buffer at
 * stride 16, which is the descriptor's own size in the table. The high word is
 * the one the runner's stage workspace mapped at (2); it does not change the
 * resource words, but the compile it is recorded from has to match the one the
 * console ran.
 *
 * Usage: compute-resources <dispatch.spv>
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psbc_compile.h"

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return NULL;
    }
    const long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return NULL;
    }
    uint8_t *data = malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length)
    {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <dispatch.spv>\n", argv[0]);
        return 2;
    }
    size_t size = 0;
    uint8_t *spirv = read_file(argv[1], &size);
    if (spirv == NULL || size % 4 != 0)
    {
        free(spirv);
        return 1;
    }

    psbc_init();
    PsbcCompileOptions options;
    memset(&options, 0, sizeof(options));
    options.target = PSBC_TARGET_PS5;
    options.stage = PSBC_STAGE_COMPUTE;
    options.entrypoint = "main";
    options.optimise = true;
    options.address32_hi = 2;
    options.descriptor_bindings[0] =
        (PsbcDescriptorBinding){0, 0, PSBC_DESCRIPTOR_STORAGE_BUFFER, 1, 0, 16};
    options.descriptor_binding_count = 1;

    PsbcShaderOutput output;
    memset(&output, 0, sizeof(output));
    const PsbcResult result =
        psbc_compile_shader((const uint32_t *)spirv, size, &options, &output);
    /* The fork reports the dispatch's resource words in the shader register table,
     * as dword offsets from SI_SH_REG_OFFSET (0xb000) rather than as the raw
     * addresses a dispatch programs: COMPUTE_PGM_RSRC1 is 0xb848 and offset
     * 0x212, RSRC2 0xb84c and 0x213, RSRC3 0xb8a0 and 0x228. The 0.2.0-era
     * compiler exported them as metadata fields instead
     * (tooling/psbc/patch-compute-metadata.py, dropped when this repository
     * migrated to the fork), which is why this recorder reads the table. */
    uint32_t rsrc[3] = {0, 0, 0};
    bool have[3] = {false, false, false};
    for (uint32_t i = 0; i < output.metadata.shader_register_count; ++i)
    {
        const PsbcRegisterWrite *const write = &output.metadata.shader_registers[i];
        if (write->offset == 0x212)
        {
            rsrc[0] = write->value;
            have[0] = true;
        }
        else if (write->offset == 0x213)
        {
            rsrc[1] = write->value;
            have[1] = true;
        }
        else if (write->offset == 0x228)
        {
            rsrc[2] = write->value;
            have[2] = true;
        }
    }
    if (result != PSBC_RESULT_OK || !have[0] || !have[1] || !have[2] ||
        output.machine_code_size == 0)
    {
        fprintf(stderr, "%s: compile failed: %s\n", argv[1], psbc_result_string(result));
        psbc_free_output(&output);
        psbc_shutdown();
        free(spirv);
        return 1;
    }

    printf("rsrc1 0x%08x\n", rsrc[0]);
    printf("rsrc2 0x%08x\n", rsrc[1]);
    printf("rsrc3 0x%08x\n", rsrc[2]);
    printf("wave_size %u\n", output.metadata.compute_wave_size);
    printf("workgroup %u %u %u\n", output.metadata.compute_workgroup_size[0],
           output.metadata.compute_workgroup_size[1],
           output.metadata.compute_workgroup_size[2]);

    psbc_free_output(&output);
    psbc_shutdown();
    free(spirv);
    return 0;
}
