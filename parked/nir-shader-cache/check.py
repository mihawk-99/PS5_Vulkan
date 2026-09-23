#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check stable NIR keys and fresh-process cache reuse against the driver."""
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
obj = root / 'build/nir-cache-check.o'
exe = root / 'build/nir-cache-check'
subprocess.run(['gcc', *['-I' + str(root/p) for p in includes], *flags, '-c',
                str(root/'parked/nir-shader-cache/check.c'), '-o', str(obj)], cwd=tree, check=True)
libs = ['build/driver/host/libps5vk.a', '.deps/native/vulkan-runtime/lib/libvk_runtime.a',
        'build/driver/host/ps5_host.o', 'build/driver/host/agc_host.o',
        'build/driver/host/ps5_agc_package.o', 'build/driver/host/libpsbc_driver.pic.a']
subprocess.run(['g++', '-o', str(exe), str(obj), '-Wl,--start-group', *libs,
                '-Wl,--end-group', '-pthread', '-lm'], check=True)
import os, tempfile
work = Path(tempfile.mkdtemp(prefix='nir-cache-', dir=root/'build'))
for run in ('cold', 'warm', 'disabled'):
    with (work/(run+'.log')).open('w') as log:
        subprocess.run([str(exe), str(work/(run+'.bin'))], check=True, stdout=log, stderr=log,
            env={**os.environ, 'PS5VK_SHADER_CACHE_DIR': str(work/'cache') if run != 'disabled' else ''})
cold = (work/'cold.log').read_text(); warm = (work/'warm.log').read_text()
assert 'shader cache stored' in cold
assert 'shader cache hit' in warm and 'compile start:' not in warm
assert next(l for l in cold.splitlines() if l.startswith('key ')) == next(l for l in warm.splitlines() if l.startswith('key '))
assert (work/'cold.bin').read_bytes() == (work/'warm.bin').read_bytes() == (work/'disabled.bin').read_bytes()
print('PASS: fresh-process NIR cache skips compilation with byte-identical output and stable keys')
print('logs:', work.relative_to(root))
