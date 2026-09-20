#!/usr/bin/env bash
# PS5 Vulkan - compile two fragment varyings without aliasing their input slots.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tree="$root/.deps/work/psbc-ps5/third_party/opengnm-psbc"
work="$root/build/fragment-inputs"
mkdir -p "$work"
gcc -std=c11 -O2 -Wall -Wextra -Werror -I "$tree/libpsbc" \
    -c "$root/tooling/psbc/fragment-inputs.c" -o "$work/check.o"
g++ "$work/check.o" "$tree/libpsbc.pic.a" -pthread -lm -o "$work/check"
# Dense inputs reproduce RetroArch; sparse locations also need dense hardware slots.
for pair in '0 1' '3 7'; do
    read -r uv color <<< "$pair"
    cat > "$work/check.frag" <<GLSL
#version 450
layout(location = $uv) in vec2 uv;
layout(location = $color) in vec4 color;
layout(location = 0) out vec4 result;
layout(set = 0, binding = 0) uniform sampler2D tex;
void main() { result = color * texture(tex, uv); }
GLSL
    glslangValidator -V "$work/check.frag" -o "$work/check.spv" >/dev/null
    "$work/check" "$work/check.spv" "$uv" "$color"
done
