/* PS5 Vulkan - compile shaders one after another in one process.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The compiler keeps state in the process it runs in -- the driver compiles
 * every pipeline of a title through one psbc_init/psbc_shutdown pair -- and the
 * project has recorded one fault that only that shape shows: after the unsigned
 * texture case's compile, the *next* compile in the same process died with a
 * SIGFPE in aco::schedule_program (Klog_Logs/v0-u16-run1.log,
 * v0-snorm-run1.log; docs/HARDWARE_FINDINGS.md). This tool is the offline
 * reproducer for that and for any other compile-order fault: it takes a queue
 * file of one compile a line
 *
 *     <spir-v path> <descriptor>
 *
 * where <descriptor> is none, tex (a combined image sampler, 48-byte entry),
 * texel (a uniform texel buffer, 16) or image (a storage image, 32), and
 * compiles each as a PS5 fragment stage with the probes' own options
 * (address32_hi 2). Every step prints its result line before the next one runs,
 * so a fault names the compile that was in flight and the one before it.
 *
 * tools/check-aco-state.sh builds this against the same compiler archive the
 * driver links and runs the recorded sequences through it; the tool itself is
 * diagnostic, not a pass/fail gate (a fault that stops happening is good news,
 * and the script says so).
 */
#include "psbc_compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum descriptor_kind {
    DESCRIPTOR_NONE = 0,
    DESCRIPTOR_TEX,
    DESCRIPTOR_TEXEL,
    DESCRIPTOR_IMAGE,
};

static enum descriptor_kind parse_descriptor(const char *name)
{
    if (strcmp(name, "none") == 0)
        return DESCRIPTOR_NONE;
    if (strcmp(name, "tex") == 0)
        return DESCRIPTOR_TEX;
    if (strcmp(name, "texel") == 0)
        return DESCRIPTOR_TEXEL;
    if (strcmp(name, "image") == 0)
        return DESCRIPTOR_IMAGE;
    fprintf(stderr, "unknown descriptor %s (none, tex, texel, image)\n", name);
    exit(2);
}

static uint32_t *read_spirv(const char *path, size_t *bytes)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    rewind(file);
    uint32_t *words = length > 0 && length % 4 == 0 ? malloc((size_t)length) : NULL;
    if (words && fread(words, 1, (size_t)length, file) != (size_t)length)
    {
        free(words);
        words = NULL;
    }
    fclose(file);
    *bytes = words ? (size_t)length : 0;
    return words;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: compile-sequence <queue file>\n");
        return 2;
    }
    FILE *queue = fopen(argv[1], "r");
    if (!queue)
    {
        perror(argv[1]);
        return 2;
    }

    psbc_init();
    char line[1024];
    unsigned step = 0;
    while (fgets(line, sizeof(line), queue))
    {
        char path[900];
        char descriptor[32];
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%899s %31s", path, descriptor) != 2)
            continue;
        const enum descriptor_kind kind = parse_descriptor(descriptor);
        size_t bytes = 0;
        uint32_t *words = read_spirv(path, &bytes);
        if (!words)
        {
            fprintf(stderr, "step %u: cannot read %s\n", step, path);
            fclose(queue);
            return 2;
        }
        PsbcCompileOptions options = {
            .target = PSBC_TARGET_PS5,
            .stage = PSBC_STAGE_FRAGMENT,
            .entrypoint = "main",
            .optimise = true,
            .address32_hi = 2,
        };
        switch (kind)
        {
        case DESCRIPTOR_TEX:
            options.descriptor_binding_count = 1;
            options.descriptor_bindings[0] =
                (PsbcDescriptorBinding){0, 0, PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER, 1, 0, 48};
            break;
        case DESCRIPTOR_TEXEL:
            options.descriptor_binding_count = 1;
            options.descriptor_bindings[0] =
                (PsbcDescriptorBinding){0, 0, PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER, 1, 0, 16};
            break;
        case DESCRIPTOR_IMAGE:
            options.descriptor_binding_count = 1;
            options.descriptor_bindings[0] =
                (PsbcDescriptorBinding){0, 0, PSBC_DESCRIPTOR_STORAGE_IMAGE, 1, 0, 32};
            break;
        case DESCRIPTOR_NONE:
            break;
        }
        PsbcShaderOutput output = {0};
        printf("step %u: %s (%s) ... ", step, path, descriptor);
        fflush(stdout);
        const PsbcResult result = psbc_compile_shader(words, bytes, &options, &output);
        printf("result=%d code=%zu\n", (int)result, output.machine_code_size);
        fflush(stdout);
        psbc_free_output(&output);
        free(words);
        step++;
    }
    fclose(queue);
    psbc_shutdown();
    printf("compiled %u shader(s) in one process without a fault\n", step);
    return 0;
}
