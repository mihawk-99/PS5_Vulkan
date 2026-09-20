/* PS5 Vulkan - regression for distinct fragment input locations.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "psbc_compile.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file)
        return 2;
    fseek(file, 0, SEEK_END);
    const long bytes = ftell(file);
    rewind(file);
    uint32_t *words = bytes > 0 && bytes % 4 == 0 ? malloc((size_t)bytes) : NULL;
    if (!words || fread(words, 1, (size_t)bytes, file) != (size_t)bytes)
        return 2;
    fclose(file);
    const PsbcCompileOptions options = {
        .target = PSBC_TARGET_PS5,
        .stage = PSBC_STAGE_FRAGMENT,
        .entrypoint = "main",
        .optimise = true,
        .address32_hi = 2,
        .spi_shader_col_format = 0x99999994,
        .descriptor_binding_count = 1,
        .descriptor_bindings = {{0, 0, PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER, 1, 0, 48}},
    };
    PsbcShaderOutput output = {0};
    psbc_init();
    const PsbcResult result = psbc_compile_shader(words, (size_t)bytes, &options, &output);
    const PsbcShaderMetadata *metadata = &output.metadata;
    const uint32_t first = 15u + (uint32_t)strtoul(argv[2], NULL, 10);
    const uint32_t second = 15u + (uint32_t)strtoul(argv[3], NULL, 10);
    const int ok = result == PSBC_RESULT_OK && output.machine_code_size > 0 &&
                   !(metadata->unresolved_fields & PSBC_UNRESOLVED_AGC_LINKAGE) &&
                   metadata->input_semantic_count == 2 && metadata->input_semantics[0] == first &&
                   metadata->input_semantics[1] == second;
    printf("fragment inputs: result=%d count=%u semantics=%u,%u expected=%u,%u linkage=%s\n",
           result, metadata->input_semantic_count, metadata->input_semantics[0],
           metadata->input_semantics[1], first, second,
           (metadata->unresolved_fields & PSBC_UNRESOLVED_AGC_LINKAGE) ? "unresolved" : "resolved");
    psbc_free_output(&output);
    psbc_shutdown();
    free(words);
    return ok ? 0 : 1;
}
