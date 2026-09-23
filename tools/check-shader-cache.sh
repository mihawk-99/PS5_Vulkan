#!/usr/bin/env bash
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
# After tools/build-driver.sh and tools/check-driver.sh c4_texture.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
work=$(mktemp -d "$root/build/shader-cache-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
export PS5VK_SHADER_CACHE_DIR="$work/cache"
# New compiler options must not silently disappear from the persistent key.
python3 - <<'PY_FIELDS'
import re
from pathlib import Path
header = Path(".deps/native/psbc/include/psbc_compile.h").read_text()
body = header.split("} PsbcCompileOptions;")[0].rsplit("typedef struct {", 1)[1]
body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
fields = set(re.findall(r"\b(\w+)\s*(?:\[[^]]+\])?\s*;", body))
source = Path("driver/ps5vk_shader_cache.c").read_text()
keyed = set(re.findall(r"FIELD\((\w+)", source)) | set(re.findall(r"options->(\w+)", source))
assert not fields - keyed, "compile options missing from cache key: " + str(fields - keyed)
print("PASS: every compiler-option field participates in the key")
PY_FIELDS
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
    -Idriver -I.deps/native/psbc/include -Ibuild/driver/generated \
    -I.deps/work/psbc-ps5/third_party/opengnm-psbc/src \
    jobs/shader-cache/cache-test.c driver/ps5vk_shader_cache.c \
    build/driver/host/libpsbc_driver.pic.a -lstdc++ -lpthread -lm -o "$work/cache-test"
"$work/cache-test"
export PS5VK_PROBES="$root/probes"
export PS5_HOST_REPLAY="$root/build/driver/check/c4-texture.replay"
for run in cold warm; do
    PS5VK_PIPELINE_DUMP="$work/$run" build/driver/check/c4_texture_direct > "$work/$run.log" 2>&1
done
grep -q 'shader cache stored' "$work/cold.log"
grep -q 'shader cache hit' "$work/warm.log"
if grep -q 'compile start:.*nir=(nil)' "$work/warm.log"; then
    echo 'FAIL: a warm SPIR-V shader still compiled' >&2; exit 1
fi
cmp "$work/cold-vertex.bin" "$work/warm-vertex.bin"
cmp "$work/cold-pixel.bin" "$work/warm-pixel.bin"
echo "PASS: fresh-process warm launch skips SPIR-V compilation; vertex/pixel packages identical"
