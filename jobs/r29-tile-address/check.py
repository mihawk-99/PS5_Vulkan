#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the CPU pixel check against the actual driver archive."""
from pathlib import Path
import shlex
import subprocess
root = Path.cwd()
flags = shlex.split(Path('build/driver/host/flags').read_text())
flags = flags[flags.index('gcc') + 1:flags.index('headers')]
tree = root / '.deps/work/psbc-ps5/third_party/opengnm-psbc'
includes = ['driver', 'build/driver/generated', '.deps/native/vulkan-runtime/include/vulkan/runtime',
            '.deps/native/vulkan-runtime/include/vulkan/util', '.deps/native/vulkan-runtime/include',
            '.deps/native/psbc/include']
baseline = subprocess.check_output(['git', 'show', '9f9f395:driver/ps5vk_image.c'], text=True)
start = baseline.index('struct ps5vk_tiled_term {')
end = baseline.index('/* The two-byte depth map', start)
legacy = baseline[start:end].replace('static uint64_t\nps5vk_tiled_texel_offset',
                                     'uint64_t\nlegacy_tiled_texel_offset')
legacy_path = root / 'build/r29-legacy-map.c'
legacy_path.write_text('#include <stdint.h>\n#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))\n'
                      '#define DIV_ROUND_UP(x,y) (((x)+(y)-1)/(y))\n'
                      '#define PS5VK_TILE_BYTES UINT64_C(0x10000)\n' + legacy)
legacy_obj = root / 'build/r29-legacy-map.o'
subprocess.run(['gcc', '-O2', '-c', str(legacy_path), '-o', str(legacy_obj)], check=True)
obj = root / 'build/r29-half-blit-check.o'
exe = root / 'build/r29-half-blit-check'
subprocess.run(['gcc', *['-I' + str(root/p) for p in includes], *flags, '-c',
                str(root/'jobs/r29-tile-address/check.c'), '-o', str(obj)], cwd=tree, check=True)
libs = ['build/driver/host/libps5vk.a', '.deps/native/vulkan-runtime/lib/libvk_runtime.a',
        'build/driver/host/ps5_host.o', 'build/driver/host/agc_host.o',
        'build/driver/host/ps5_agc_package.o', 'build/driver/host/libpsbc_driver.pic.a']
subprocess.run(['g++', '-o', str(exe), str(obj), str(legacy_obj), '-Wl,--start-group', *libs,
                '-Wl,--end-group', '-pthread', '-lm'], check=True)
subprocess.run([str(exe)], check=True)
