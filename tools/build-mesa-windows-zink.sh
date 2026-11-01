#!/usr/bin/env bash
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build PE64 WGL/Zink against Vulkan. Optional frontend; no Wine build dependency.
# No fetches: use the pinned Mesa fork and local compiler archive.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${1:-$root/.deps/mesa-windows-zink}
mesa_fork=${2:-$root/../PS5_Mesa}
mingw_archive=${3:?usage: build-mesa-windows-zink.sh WORK MESA_FORK LLVM_MINGW_ARCHIVE}
mkdir -p "$work"
work=$(cd -- "$work" && pwd)
mesa_fork=$(cd -- "$mesa_fork" && pwd)
mingw_archive=$(realpath "$mingw_archive")
revision=a8800d9130583fbd4a436c4c64ab9e33b98edf10
printf '%s  %s\n' bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21 "$mingw_archive" | sha256sum --check --status
mkdir -p "$work"
mingw=$work/toolchain
if [ ! -f "$mingw/.archive-sha256" ] || [ "$(cat "$mingw/.archive-sha256")" != bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21 ]; then
    mkdir -p "$mingw"
    tar -xJf "$mingw_archive" -C "$mingw" --strip-components=1
    printf '%s\n' bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21 > "$mingw/.archive-sha256"
fi
if [ ! -f "$work/source/.revision" ] || [ "$(cat "$work/source/.revision")" != "$revision" ]; then
    staging=$(mktemp -d "$work/source.XXXXXX")
    git -C "$mesa_fork" archive "$revision" | tar -x -C "$staging"
    printf '%s\n' "$revision" > "$staging/.revision"
    rm -rf "$work/source"
    mv "$staging" "$work/source"
fi
[ "$(cat "$work/source/VERSION")" = 26.2.0 ]
cat > "$work/cross.ini" <<CROSS
[binaries]
c = '$mingw/bin/x86_64-w64-mingw32-clang'
cpp = '$mingw/bin/x86_64-w64-mingw32-clang++'
ar = '$mingw/bin/llvm-ar'
strip = '$mingw/bin/llvm-strip'
windres = '$mingw/bin/llvm-windres'
pkg-config = 'false'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
CROSS
export PATH="$mingw/bin:$PATH"
reconfigure=()
[ ! -f "$work/build/build.ninja" ] || reconfigure=(--reconfigure)
meson setup "${reconfigure[@]}" "$work/build" "$work/source" --cross-file "$work/cross.ini" \
    --wrap-mode=nofallback --buildtype=release --default-library=static \
    -Dplatforms=windows -Dgallium-drivers=zink -Dvulkan-drivers= -Dopengl=true \
    -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dllvm=disabled -Dzlib=disabled -Dzstd=disabled -Dshader-cache=disabled \
    -Dxmlconfig=disabled -Dexpat=disabled -Dlibunwind=disabled -Dvalgrind=disabled \
    -Dvideo-codecs= -Dbuild-tests=false -Dtools= -Dgallium-va=disabled -Dgallium-rusticl=false \
    -Dc_link_args=-Wl,--no-insert-timestamp -Dcpp_link_args=-Wl,--no-insert-timestamp
ninja -C "$work/build" -j "${JOBS:-12}" \
    src/gallium/targets/libgl-gdi/opengl32.dll src/gallium/targets/wgl/libgallium_wgl.dll
python3 - "$root" "$work" <<'PY'
import hashlib,json,sys
from pathlib import Path
root,work=map(Path,sys.argv[1:])
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
files={p.name:digest(p) for p in work.glob('build/src/gallium/targets/*/*.dll')}
manifest={'mesa_version':'26.2.0','mesa_revision':'a8800d9130583fbd4a436c4c64ab9e33b98edf10',
          'llvm_mingw':'20260922-ucrt','build_script':digest(root/'tools/build-mesa-windows-zink.sh'),
          'architecture':'x86_64','driver':'zink','files':files,'console_validated':False}
manifest['zink_source_sha256']=digest(work/'source/src/gallium/drivers/zink/zink_kopper.c')
# Every Zink source the fork changes: the present mode (zink_kopper.c) and persistently mapped buffers.
zink=work/'source/src/gallium/drivers/zink'
manifest['zink_sources_sha256']={n:digest(zink/n) for n in
    ('zink_kopper.c','zink_bo.h','zink_descriptors.c','zink_resource.c')}
(work/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Built PE64 Mesa WGL/Zink:',', '.join(files))
PY
