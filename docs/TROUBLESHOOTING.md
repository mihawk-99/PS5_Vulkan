# Troubleshooting

## The native toolchain is missing

Run the read-only host check:

```bash
make doctor
```

Confirm the required commands resolve:

```bash
for command in clang clang++ clang-format clang-tidy llvm-ar llvm-ranlib \
    make python3 git curl wget unzip tar sha256sum; do
  command -v "$command" >/dev/null || echo "missing: $command"
done
```

On CachyOS, install what is missing with:

```bash
sudo pacman -S --needed base-devel clang llvm lld make python python-pip \
  git curl wget unzip tar pkgconf
```

Arch ships unversioned command names, so `clang` and `llvm-ar` are the
expected binaries. The cross-compiler that produces PS5 output lives in
`.deps/native/ps5-payload-sdk/bin` and is downloaded by the build; it is not a
host package.

## The generated `libc.prx` is missing or has the wrong hash

Run the source reproducer, which verifies both release digests:

```bash
make libc
```

Normal `make` builds it automatically when absent. Do not replace it with a
module extracted from a game or firmware.

## The linker reports unresolved symbols

Check spelling, C versus C++ linkage, and whether the needed static archive is
listed in `APP_STATIC_ARCHIVES` or the relevant `PACBREW_*` variable. Platform
imports must exist in the public SDK stubs under
`.deps/native/ps5-payload-sdk/target/lib`. Do not silence unresolved symbols;
update the SDK or provide a legitimate native implementation.

## Native dependency bootstrap fails

The first build needs network access to download the hash-pinned public PS5
payload SDK and upstream zlib source archive. Retry:

```bash
make deps
```

The script writes only to `.deps/native/` and never installs packages globally.

## Optional package setup fails

- `.ffpkg` requires Git, the .NET SDK 8 or newer, and network access on first
  use. UFS2Tool and its build output are stored under `.deps/UFS2Tool`.
- `.ffpfsc` requires Git and Python 3.9 or newer with `venv` support. MkPFS and
  its isolated environment are stored under `.deps/MkPFS`.
- Folder output has neither optional dependency. Use `make app` to isolate
  packaging from compilation.

Nothing is installed globally by these optional bootstrappers.

## Mesa's Vulkan generators fail to import mako

`tools/build-vulkan-runtime.sh` runs Mesa's Python generators, which need
`mako` and `markupsafe`:

```bash
sudo pacman -S --needed python-mako python-markupsafe
python3 -c 'import mako, markupsafe; print(mako.__version__)'
```

If you keep the packages somewhere else, point `PS5VK_MAKO_PATH` at a
directory that contains them. `tools/mesa-python.sh` reports the version it
found and fails with a clear message when nothing is importable.

## FTP deployment fails

- Confirm `PS5_HOST` identifies the intended console and its FTP service is
  already running on `FTP_PORT` (default `2121`).
- Run `make deploy PS5_HOST=192.0.2.1 DEPLOY_DRY_RUN=1` to validate the local
  build and destination without sending a network request.
- Fully close the previous title and any crash dialog before replacing its
  files. Never launch while deployment is still running.
- A remaining hidden `.upload` file indicates that a transfer or rename did
  not finish. Rerunning deployment safely overwrites that temporary file.
- Folder deployment intentionally does not delete remote files absent from the
  new build. Manually clean the title directory if a removed asset or module
  must disappear.
- Do not leave both a title folder and an image with the same title ID under
  active scan paths. ShadowMountPlus may prefer its existing mounted source.
- Wait for the mount service to report the title ready before launching. The
  Make target uploads only; it does not launch the app.

## The title does not appear

- Confirm `dist/<TITLE_ID>/sce_sys/param.json` and `icon0.png` exist.
- Confirm another title is not still active in the loader.
- Use a title ID not already registered by another application.
- Wait for the directory loader's explicit ready/installed message.
- Stage the whole title directory, not only `eboot.bin`.

## The icon, background, or selection audio does not update

- Run `make`; both the normal build and `make assets-check` validate the tracked
  presentation assets before compiling.
- Confirm `icon0.png`, `pic0.dds`, and `pic1.dds` reached
  `dist/<TITLE_ID>/sce_sys/`.
- Selection and launch pictures must be 3840x2160 DX10 DDS files using BC7 UNORM. A PNG
  renamed to `.dds` is not sufficient.
- Audio must be ATRAC9 in a RIFF container named exactly `snd0.at9`; renaming
  MP3 or AAC input does not convert it.
- The RIFF must contain one `smpl` loop. If selecting the app stops default
  home-screen music but remains silent, inspect the chunk list.
- `Base.BgmController: Invalid file size` means Shell rejected the file. The
  observed limit is 2,097,152 bytes (2 MiB), not a fixed duration. At stereo
  192 kb/s, keep input at or below 4,193,024 samples (87.354666667 seconds) so
  frame padding stays below the ceiling.
- Presentation metadata may be cached for an already registered title. Follow
  the loader's documented refresh procedure after structural changes.
- Retail-style custom logos and descriptions are Internet catalog metadata,
  not package assets for a synthetic homebrew concept.

## The app immediately crashes

- Do not return from `main` or call an exit function.
- Keep the generated runtime digest unchanged while testing the baseline.
- Keep the default FSELF magic and SDK pair until the baseline launches.
- Inspect the produced module with the repository's own validator, which reports
  the OS/ABI, ELF type, program headers, the flags-zero linking segment, the
  process-parameter record, and the FSELF container layout:

  ```bash
  make inspect
  make inspect INSPECT_FILE=dist/<TITLE_ID>/eboot.bin
  ```

  `StaticErrors` must be `0`; the command exits non-zero otherwise. With no
  argument it checks the title named by `sce_sys/param.json`. The converter's
  own summary is also available from the build output and from
  `build/host/ps5-native-tool self --inspect --file …`.
- Consult loader diagnostics; the home-screen message alone is not a root
  cause.

## `make lint` reports formatting or attribution

`make lint` runs `clang-format`, `clang-tidy`, and the header gate. The
formatter check never rewrites files, so fix formatting with:

```bash
make format
git diff --stat
```

Review the result as a whitespace-only change before keeping it. The header
gate requires every code, script, workflow, and manifest file to carry both a
copyright line and `SPDX-License-Identifier: GPL-3.0-or-later` inside its first
20 lines; it names each file that is missing one. The copyright holder is the
file's own -- `BlackBearReloaded` for the files inherited from the
ps5-native-app-boilerplate, `Mihawk-99` for this project's, both for the ones
derived from the boilerplate -- and `NOTICE.md` records the split.

Formatting rules change between Clang releases, and Arch ships only the current
one, so a future `clang-format` may disagree with a file that passes today.
Refresh the whole tree in that case with `make format` rather than pinning an
older formatter, and raise the LLVM version in `.github/workflows/tooling.yml`
in the same change so the runner keeps matching the host.

## `/download0` is missing

Keep a positive `downloadDataSize` in `sce_sys/param.json`, rebuild, and stage the
new generated directory. Do not attempt to write to `/app0`.
