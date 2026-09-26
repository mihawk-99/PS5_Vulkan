# Notices

## Native build dependencies

The application build uses LLVM/Clang/lld, zlib 1.3.2, and my fork of the
public [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk)
(`../PS5_PayloadSDK`, at the revision `tools/setup-native-dependencies.sh`
pins). The fork's setup downloads SDK v0.42 after verifying SHA-256
`8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da`, then installs
the fork's headers and its PS5 platform layer over it; both are GPL-3.0-or-later
like the SDK.
It downloads zlib 1.3.2 from the upstream source archive after verifying
SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`
and compiles its static archive locally. Both dependencies remain under ignored
`.deps/native/`, retain their upstream licenses, and are not distributed by
this repository. No Sony SDK file is included.

Target C++ compilation uses the LLVM libc++ headers distributed by the public
SDK. Those headers retain the Apache-2.0 WITH LLVM-exception license recorded
upstream. Titles do not dynamically load libc++ or libc++abi, and titles
without the shader compiler link neither.

The test runner built with `AGC_SHADER_COMPILER=1` statically links the
shader compiler and the runtime its C++ code needs (docs/M5_PHASE_A.md):
- opengnm-psbc (Mesa NIR, SPIR-V to NIR and ACO; MIT) from the ps5-opengl SDK's
  pinned tree, built by `tools/build-psbc-ps5.sh`
- ps5-opengl's C shader package writer (GPL-3.0-or-later)
- only the archive members it references from the public SDK's libc++,
  libc++abi and libunwind, and from Clang's builtins (all Apache-2.0 WITH
  LLVM-exception); `tools/check-psbc-link.sh` lists them

These build inputs stay under ignored `.deps/` and the SDK checkout, and are
not distributed by this repository.

The Vulkan driver work (Milestone 5 Phase B, docs/M5_PHASE_B.md) builds Mesa's
common Vulkan runtime (`src/vulkan/runtime` and `src/vulkan/util`, MIT) and
uses its `include/drm-uapi` headers (MIT) from the Mesa 26.2.0 release.
`tools/fetch-mesa.sh` downloads that release from archive.mesa3d.org and
verifies the SHA-256 the ps5-opengl SDK pins. The archive, its sources and
the resulting libraries stay under the ignored `.deps/` cache and are not
distributed by this repository. The window-system stubs in
`tooling/vulkan-runtime/wsi/` are project-authored.

## Presentation assets

`tools/prepare-assets.sh` converts developer-owned artwork with FFmpeg and
[richgel999/bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (MIT, Richard
Geldreich, revision `b9438627`), fetched by
`tools/setup-asset-dependencies.sh` into the ignored `.deps/native/bc7enc_rdo`
cache and built locally; the encoder is not distributed by this repository.
The generated DDS files carry the same DX10 BC7 profile as the committed,
hardware-validated assets, so no Microsoft `texconv`, Wine, or Windows tool is
required.

The project’s PS5 ELF converter and FSELF writer are independently authored
GPL-3.0-or-later code. SharpProspero was a useful public format reference during
development but is not fetched, copied, linked, or required by the build.

## Host test dependency

The host unit-test target downloads
[GoogleTest](https://github.com/google/googletest) 1.17.0 after verifying
SHA-256 `65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c`.
It remains under ignored `.deps/test/`, retains its BSD-3-Clause license, and
is not linked into any PS5 application, runtime, or package artifact.

## Optional PacBrew dependencies

When selected through `PACBREW_*` build variables, the build downloads the prebuilt ports image
from [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
release `v0.40.2`, verifies its published SHA-256, and extracts only the
`target/user/homebrew` prefix under ignored `.deps/pacbrew/`. It does not
replace the pinned SDK or install files globally. PacBrew recipes and every
linked third-party library retain their upstream licenses; applications must
review those terms before redistribution.

## Optional UFS2Tool dependency

When `.ffpkg` output is requested, the platform bootstrapper fetches
[SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) at commit
`b5307a60d5b4e3a68ba680e0e33cfadf05017c77` into the ignored
`.deps/UFS2Tool` cache and builds it with the host .NET SDK. UFS2Tool is
BSD-2-Clause software and is not distributed by this repository.

## Optional MkPFS dependency

When `.ffpfsc` output is requested, the platform bootstrapper fetches
[PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) at commit
`6cb8313dfe0c988ac52617794553f343243d3a56` into the ignored `.deps/MkPFS`
cache and installs its Python dependencies into an ignored virtual environment
there. MkPFS and its dependencies retain their own licenses and are not
distributed by this repository.

## Independently authored runtime shim

`tooling/native/libc_builder.cpp` and the manifests under
`tooling/native/runtime/` are independently authored for this project and
licensed under GPL-3.0-or-later. The generated `runtime/libc.prx` contains
project-authored compatibility stubs, startup code, and semantic loader
metadata. It contains no Sony runtime implementation.

## This project's own code

Everything in this repository that is neither inherited from the
ps5-native-app-boilerplate nor taken from another upstream is Copyright (C) 2026
Mihawk-99 and licensed under GPL-3.0-or-later: the probe harness, the Vulkan
driver and runtime sources, the shader probes, the console tooling, the tests and
the documents. Headers name the holder per file, and a file derived from the
boilerplate names both.

Original ps5-native-app-boilerplate code is Copyright (C) 2026
BlackBearReloaded and licensed under GPL-3.0-or-later. Source and script files
carry matching SPDX identifiers.

## Original presentation assets

The BlackBear icon, selection artwork, and default selection track
`sce_sys/snd0.at9` are original assets supplied by BlackBearReloaded, Copyright
(C) 2026 BlackBearReloaded, and distributed under GPL-3.0-or-later. The track
is titled `Night Drive`.

No proprietary runtime module, encryption key, or game file is included.
