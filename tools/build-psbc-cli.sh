#!/usr/bin/env bash
# PS5 Vulkan - Build the probe shader compiler CLI.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ps5-opengl's vendored opengnm-psbc library (libpsbc) already accepts
# per-MRT SPI_SHADER_COL_FORMAT export formats
# (PsbcCompileOptions.spi_shader_col_format). ps5-opengl's gallium runtime
# passes the format Mesa's ac_choose_spi_color_formats picks for each render
# target. That choice also shapes the pixel shader's ACO output code, not just
# a register. The standalone CLI has no option for it and always compiles for
# 32_ABGR exports.
#
# This script copies the CLI (cmd/psbc/main.c, MIT licence) out of the patched
# compiler work copy, adds two options and links it against the same work
# copy's host archive with the SDK's host compiler flags:
# - --color-format sets the per-MRT export formats.
# - --agc-package writes the compiled shader as an AGC package through
#   ps5-opengl's C writer (src/platform/ps5_agc_package.c), the writer a
#   console-side compiler uses (docs/M5_PHASE_A.md).
# The library comes from the work copy, not the SDK's prebuilt libpsbc.a,
# because the descriptor types the driver accepts and the CLI can name are the
# ones tooling/psbc/patch-*.py add there (docs/BLOCKERS.md): a probe shader that
# fetches a texel buffer has to be compiled by a library whose descriptor set
# layout gives that binding its 16-byte entry. Apart from those patches the
# library and all other options are unchanged, so without --color-format the
# probe CLI produces the SDK binary's output byte for byte.
#
# Run from the repository root:  bash tools/build-psbc-ps5.sh && bash tools/build-psbc-cli.sh
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tools/sdk-root.sh
source "$root/tools/sdk-root.sh"
sdk=$(cd -- "$(ps5vk_opengl_sdk "$root")" && pwd)
psbc_work=${PSBC_PS5_WORK:-$root/.deps/work/psbc-ps5}
tree="$psbc_work/third_party/opengnm-psbc"
work="$root/build/psbc-cli"
output="$root/build/host/opengnm-psbc-probe"

[[ -f $tree/cmd/psbc/main.c ]] ||
    { echo "missing the compiler work copy $tree; run tools/build-psbc-ps5.sh" >&2; exit 2; }
[[ -f $tree/libpsbc/psbc_compile.c ]] ||
    { echo "missing the compiler work copy $tree; run tools/build-psbc-ps5.sh" >&2; exit 2; }
mkdir -p "$work" "$(dirname "$output")"

# The same host archive the PC driver links (tools/build-driver.sh), built from
# the work copy: position-independent, so nothing here depends on the SDK's
# prebuilt host library's descriptors.
host_mak="$sdk/toolchain/opengnm-psbc-host.mak"
pic_log="$work/build-psbc-pic.log"
if ! make -C "$tree" -f "$root/tooling/psbc/Makefile.opengnm-psbc-host-pic" -j"$(nproc)" \
        CONFIG="$host_mak" libpsbc CC=gcc CXX=g++ > "$pic_log" 2>&1; then
    grep -E 'error:' "$pic_log" | head -n 40
    echo "the compiler work copy's host archive failed to build; see $pic_log" >&2
    exit 1
fi
echo "host archive: $tree/libpsbc.pic.a ($(stat -c %s "$tree/libpsbc.pic.a") bytes)"

python3 - "$tree/cmd/psbc/main.c" "$work/main.c" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()

def replace(old: str, new: str) -> None:
    global source
    if source.count(old) != 1:
        sys.exit(f"upstream CLI changed; anchor must occur exactly once: {old!r}")
    source = source.replace(old, new)

replace("\tuint32_t address32_hi;\n",
        "\tuint32_t address32_hi;\n"
        "\tuint32_t spi_shader_col_format;\n")
replace('\t\t} else if (!strcmp(curarg, "--vertex-attribute")) {\n',
        '\t\t} else if (!strcmp(curarg, "--color-format")) {\n'
        '\t\t\tchar* end = NULL;\n'
        '\t\t\tunsigned long value = i + 1 < argc ? strtoul(argv[++i], &end, 0) : 0;\n'
        '\t\t\tif (!end || *end || value > UINT32_MAX)\n'
        '\t\t\t\tres.parse_error = true;\n'
        '\t\t\telse\n'
        '\t\t\t\tres.spi_shader_col_format = (uint32_t)value;\n'
        '\t\t} else if (!strcmp(curarg, "--vertex-attribute")) {\n')
replace('\t    "\\t--address32-hi [value] -- Upper 32 bits for 32-bit GPU pointers\\n"\n',
        '\t    "\\t--address32-hi [value] -- Upper 32 bits for 32-bit GPU pointers\\n"\n'
        '\t    "\\t--color-format [value] -- Per-MRT SPI_SHADER_COL_FORMAT nibbles (0 = legacy 32_ABGR)\\n"\n')
replace("\t    .address32_hi = opts.address32_hi,\n",
        "\t    .address32_hi = opts.address32_hi,\n"
        "\t    .spi_shader_col_format = opts.spi_shader_col_format,\n")
replace('#include "psbc_compile.h"\n',
        '#include "psbc_compile.h"\n'
        '#include "ps5_agc_package.h"\n')
replace("\tconst char* metadatafile;\n",
        "\tconst char* metadatafile;\n"
        "\tconst char* agcpackagefile;\n")
replace('\t\t} else if (!strcmp(curarg, "--metadata")) {\n',
        '\t\t} else if (!strcmp(curarg, "--agc-package")) {\n'
        '\t\t\tif (i + 1 < argc) {\n'
        '\t\t\t\tres.agcpackagefile = argv[i + 1];\n'
        '\t\t\t}\n'
        '\t\t} else if (!strcmp(curarg, "--metadata")) {\n')
replace('\t    "\\t--metadata [path] -- Write typed hardware metadata as JSON\\n"\n',
        '\t    "\\t--metadata [path] -- Write typed hardware metadata as JSON\\n"\n'
        '\t    "\\t--agc-package [path] -- Also write an AGC package with ps5-opengl\'s C writer\\n"\n')
replace("\tif (opts.metadatafile)\n\t\twrite_metadata(opts.metadatafile, &output);\n",
        "\tif (opts.metadatafile)\n\t\twrite_metadata(opts.metadatafile, &output);\n"
        "\tif (opts.agcpackagefile) {\n"
        "\t\tuint8_t* package = NULL;\n"
        "\t\tsize_t package_size = 0;\n"
        "\t\tconst int package_result = ps5_agc_package_build(&output, 1, &package, &package_size);\n"
        "\t\tFILE* p = package_result == 0 ? fopen(opts.agcpackagefile, \"wb\") : NULL;\n"
        "\t\tconst bool written = p && fwrite(package, 1, package_size, p) == package_size;\n"
        "\t\tif (p)\n"
        "\t\t\tfclose(p);\n"
        "\t\tfree(package);\n"
        "\t\tif (!written) {\n"
        "\t\t\tpsbc_free_output(&output);\n"
        "\t\t\tfatalf(\"Failed to write AGC package (writer result %d)\", package_result);\n"
        "\t\t}\n"
        "\t}\n")

header = ("/* PS5 Vulkan probe copy of opengnm-psbc cmd/psbc/main.c (MIT licence), generated\n"
          " * by tools/build-psbc-cli.sh: adds --color-format for per-MRT colour export\n"
          " * formats and --agc-package for ps5-opengl's C AGC package writer.\n"
          " * Everything else is upstream. */\n")
pathlib.Path(sys.argv[2]).write_text(header + source)
PY

# Reuse the SDK's host flags exactly, evaluated from its make configuration.
cflags=$(cd "$tree" && make -s --no-print-directory \
    -f "$host_mak" \
    -f <(printf 'print-cflags:\n\t@echo $(CFLAGS)\n') print-cflags)
platform="$sdk/src/platform"
[[ -f $platform/ps5_agc_package.c ]] ||
    { echo "missing C package writer: $platform/ps5_agc_package.c" >&2; exit 2; }
(cd "$tree" && gcc $cflags -I "$platform" -c "$work/main.c" -o "$work/main.o" &&
    gcc $cflags -I "$platform" -c "$platform/ps5_agc_package.c" -o "$work/ps5_agc_package.o" &&
    g++ -o "$output" "$work/main.o" "$work/ps5_agc_package.o" libpsbc.pic.a -pthread -lm)
echo "probe compiler: $output"
