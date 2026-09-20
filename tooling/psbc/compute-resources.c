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
    if (result != PSBC_RESULT_OK || !output.metadata.compute_config_valid ||
        output.machine_code_size == 0)
    {
        fprintf(stderr, "%s: compile failed: %s\n", argv[1], psbc_result_string(result));
        psbc_free_output(&output);
        psbc_shutdown();
        free(spirv);
        return 1;
    }

    printf("rsrc1 0x%08x\n", output.metadata.compute_rsrc1);
    printf("rsrc2 0x%08x\n", output.metadata.compute_rsrc2);
    printf("rsrc3 0x%08x\n", output.metadata.compute_rsrc3);
    printf("vgprs %u\n", output.metadata.compute_num_vgprs);

    psbc_free_output(&output);
    psbc_shutdown();
    free(spirv);
    return 0;
}
