#!/usr/bin/env bash
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: LGPL-2.1-or-later
# Isolated experimental client; retains PS5_Vulkan's complete RADV link recipe.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
radv=${PS5_VULKAN_BUILD_ROOT:-$root}
out=${1:?usage: build-gpu-broker-client.sh OUTPUT SDK [WSI=1] [DRAW=0]}
sdk=${2:?SDK required}
mkdir -p "$out"
out=$(cd -- "$out" && pwd)
sdk=$(cd -- "$sdk" && pwd)
draw_flags=()
[[ ${4:-0} == 1 ]] && draw_flags=(-DPS5_GPU_DRAW)
wsi=${3:-1}
mesa_fork=${PS5_MESA_FORK:-$radv/../PS5_Mesa}
overlay_revision=2a53a8e29f20606ede9155ed5f31b6249d63e23c
install=$radv/.deps/native/radv-release
original=$install/lib/libvulkan_radeon.ps5.a
revision=1ba9874f478657ccf6da72ce55f0260164cc0bd0
# The window system layer is taken as the broker overlay (radv-gpu-broker-wsi.patch) was made: 47e9eb6 moved the lines it applies to.
wsi_revision=7b59ef27c1b09b9671bc4153c41940c3155c3af2
expected=0ffa39311881236b4872ec77a56bca417ce56c71cefca5954224b9ea8f3a1bf5
# The archive is pinned by what it was built from, not by its bytes: a release archive built on another machine
# differs byte for byte (PS5_Proton's tools/setup-pinned-radv.sh builds it from this Mesa revision and the pinned SDK).
grep -qx "revision: $revision" "$install/PROVENANCE.txt" && grep -qx 'assertions: off' "$install/PROVENANCE.txt" ||
    { echo "RADV archive provenance differs from revision $revision, release" >&2; exit 2; }
mkdir -p "$out/obj" "$out/build" "$out/patches" "$out/source/src/amd/vulkan/winsys/ps5"
for name in features local-memory client-wait wsi; do
    git -C "$mesa_fork" show "$overlay_revision:ps5/broker/radv-gpu-broker-$name.patch" > "$out/patches/radv-gpu-broker-$name.patch"
done
for source in radv_ps5_winsys.c radv_ps5_winsys.h radv_ps5_platform.h radv_ps5_cs.c radv_ps5_bo.c; do
    git -C "$mesa_fork" show "$revision:src/amd/vulkan/winsys/ps5/$source" > "$out/source/src/amd/vulkan/winsys/ps5/$source"
done
patch --batch -p1 -d "$out/source" < "$out/patches/radv-gpu-broker-features.patch"
# Device-local memory from the owner, as a discrete GPU's VRAM (docs/GRAPHICS_PROCESS_BROKER.md, the high-water mark).
patch --batch -p1 -d "$out/source" < "$out/patches/radv-gpu-broker-local-memory.patch"
# Short GPU waits: a client spins briefly and sleeps in short steps (the patch's comment has what was measured).
patch --batch -p1 -d "$out/source" < "$out/patches/radv-gpu-broker-client-wait.patch"
# Sparse ranges through the owner (PS5_Proton patch 0041): the client advertises sparse resources again.
git -C "$mesa_fork" show "$overlay_revision:ps5/broker/client-sparse-ranges-winsys.patch" > "$out/patches/client-sparse-ranges-winsys.patch"
patch --batch -p1 -d "$out/source" < "$out/patches/client-sparse-ranges-winsys.patch"
# The device-local budget follows the owner's room (PS5_Proton patch 0042).
git -C "$mesa_fork" show "$overlay_revision:ps5/broker/client-budget-winsys.patch" > "$out/patches/client-budget-winsys.patch"
patch --batch -p1 -d "$out/source" < "$out/patches/client-budget-winsys.patch"
# The GPU a Windows game is told it is, for the console model the owner says (PS5_Proton patch 0045).
git -C "$mesa_fork" show "$overlay_revision:ps5/broker/client-identity-winsys.patch" > "$out/patches/client-identity-winsys.patch"
patch --batch -p1 -d "$out/source" < "$out/patches/client-identity-winsys.patch"
cp "$root/tooling/gpu-broker/pw_gpu_identity.h" "$out/source/src/amd/vulkan/winsys/ps5/"   # PS5_Proton's, beside the winsys
# Host memory the GPU reaches, for a client in the owner's process only (Direct3D 12's existing heaps).
git -C "$mesa_fork" show "$overlay_revision:ps5/broker/client-host-memory-winsys.patch" > "$out/patches/client-host-memory-winsys.patch"
patch --batch -p1 -d "$out/source" < "$out/patches/client-host-memory-winsys.patch"
# Reuse Mesa's generated header/include flags. Strip every output/dependency
# option before compilation, so the sibling build remains read-only.
python3 - "$radv" "$out" <<'PY'
import json,shlex,subprocess,sys
from pathlib import Path
radv,out=map(Path,sys.argv[1:]);build=radv/'.deps/work/radv-build-ps5-release'
commands=json.loads((build/'compile_commands.json').read_text())
# Every translation unit must see the same patched header: the ring size (the allocator and command-ring bounds
# otherwise disagree and would corrupt shared memory) and the device-local buffers the BO code routes.
for source in ('radv_ps5_winsys.c','radv_ps5_cs.c','radv_ps5_bo.c'):
    row=next(r for r in commands if r['file'].endswith('/'+source))
    args=shlex.split(row['command']);clean=[];i=0
    while i<len(args):
        a=args[i]
        if a in ('-MQ','-MF','-o','-c'):i+=2;continue
        if a=='-MD':i+=1;continue
        clean.append(a.strip("'"));i+=1
    subprocess.run(clean+['-I'+str(build.parent/'radv-src/src/amd/vulkan/winsys/ps5'),'-c',str(out/'source/src/amd/vulkan/winsys/ps5'/source),'-o',str(out/'obj'/('winsys_ps5_'+source+'.o'))],cwd=build,check=True)
PY
cp "$original" "$out/client.a"
"$sdk/bin/llvm-ar" d "$out/client.a" winsys_ps5_radv_ps5_platform.c.o wsi_common_videoout.c.o winsys_ps5_radv_ps5_winsys.c.o winsys_ps5_radv_ps5_cs.c.o winsys_ps5_radv_ps5_bo.c.o
"$sdk/bin/llvm-ar" r "$out/client.a" "$out/obj/winsys_ps5_radv_ps5_winsys.c.o" "$out/obj/winsys_ps5_radv_ps5_cs.c.o" "$out/obj/winsys_ps5_radv_ps5_bo.c.o"
for source in client packet_io; do
    "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
        "${draw_flags[@]}" -I"$install/include" -I"$out/source/src/amd/vulkan/winsys/ps5" \
        -c "$root/tooling/gpu-broker/$source.c" -o "$out/obj/$source.o"
done
if [[ $wsi == 1 ]]; then
    mkdir -p "$out/source/src/vulkan/wsi"
    git -C "$mesa_fork" show "$wsi_revision:src/vulkan/wsi/wsi_common_videoout.c" > "$out/source/src/vulkan/wsi/wsi_common_videoout.c"
    patch --batch -p1 -d "$out/source" < "$out/patches/radv-gpu-broker-wsi.patch"
    git -C "$mesa_fork" show "$overlay_revision:ps5/broker/window-surfaces-wsi.patch" > "$out/patches/window-surfaces-wsi.patch"
    patch --batch -p1 -d "$out/source" < "$out/patches/window-surfaces-wsi.patch"
    git -C "$mesa_fork" show "$overlay_revision:ps5/broker/present-times-wsi.patch" > "$out/patches/present-times-wsi.patch"
    patch --batch -p1 -d "$out/source" < "$out/patches/present-times-wsi.patch"
    git -C "$mesa_fork" show "$overlay_revision:ps5/broker/async-present-wsi.patch" > "$out/patches/async-present-wsi.patch"
    patch --batch -p1 -d "$out/source" < "$out/patches/async-present-wsi.patch"
    git -C "$mesa_fork" show "$overlay_revision:ps5/broker/present-mode-wsi.patch" > "$out/patches/present-mode-wsi.patch"   # any present mode, presented as FIFO
    patch --batch -p1 -d "$out/source" < "$out/patches/present-mode-wsi.patch"
    git -C "$mesa_fork" show "$overlay_revision:ps5/broker/window-extent-wsi.patch" > "$out/patches/window-extent-wsi.patch"   # a window larger than the screen, clipped
    patch --batch -p1 -d "$out/source" < "$out/patches/window-extent-wsi.patch"
    git -C "$mesa_fork" show "$overlay_revision:ps5/broker/display-extent-wsi.patch" > "$out/patches/display-extent-wsi.patch"   # a display swapchain larger than the screen too
    patch --batch -p1 -d "$out/source" < "$out/patches/display-extent-wsi.patch"
    cp "$root/tooling/gpu-broker/present-wait-wsi.patch" "$out/patches/"   # FIFO held to the display, present waits when shown
    patch --batch -p1 -d "$out/source" < "$out/patches/present-wait-wsi.patch"
    cp "$root/tooling/gpu-broker/pw_present_retry.h" "$out/source/src/vulkan/wsi/"   # PS5_Proton's, beside the WSI
    python3 - "$radv" "$out" <<'PYWSI'
import json,shlex,subprocess,sys
from pathlib import Path
radv,out=map(Path,sys.argv[1:]);build=radv/'.deps/work/radv-build-ps5-release'
row=next(r for r in json.loads((build/'compile_commands.json').read_text()) if r['file'].endswith('/wsi_common_videoout.c'))
args=shlex.split(row['command']);clean=[];i=0
while i<len(args):
    a=args[i]
    if a in ('-MQ','-MF','-o','-c'):i+=2;continue
    if a=='-MD':i+=1;continue
    clean.append(a.strip("'"));i+=1
subprocess.run(clean+['-c',str(out/'source/src/vulkan/wsi/wsi_common_videoout.c'),'-o',str(out/'obj/wsi.o')],cwd=build,check=True)
PYWSI
else
    "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$install/include" \
        -c "$root/tooling/gpu-broker/headless_wsi.c" -o "$out/obj/wsi.o"
fi
"$sdk/bin/llvm-ar" r "$out/client.a" "$out/obj/client.o" "$out/obj/packet_io.o" "$out/obj/wsi.o"
python3 - "$root" "$radv" "$sdk" "$out" "$wsi" <<'PYMANIFEST'
import hashlib,json,sys
from pathlib import Path
root,radv,sdk,out=map(Path,sys.argv[1:5]);digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
files=[p for p in (root/'tooling/gpu-broker').glob('*') if p.is_file()]
manifest={'wsi':sys.argv[5]=='1','sha256':digest(out/'client.a'),
 'mesa_base_revision':'1ba9874f478657ccf6da72ce55f0260164cc0bd0',
 'mesa_overlay_revision':'2a53a8e29f20606ede9155ed5f31b6249d63e23c',
 'sdk_revision':(sdk/'.ps5-sdk-revision').read_text().strip(),
 'recipe_sha256':digest(radv/'tools/radv-link.sh'),
 'build_script_sha256':digest(root/'tools/build-gpu-broker-client.sh'),
 'source_sha256':{str(p.relative_to(root)):digest(p) for p in files},
 'patch_sha256':{p.name:digest(p) for p in (out/'patches').glob('*.patch')},
 'console_validated':False}
(out/'foundation-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Built PS5 GPU broker client archive')
PYMANIFEST
