# Milestone 5, Phase A: shader compilation on the console

Append-only run log: add new entries at the end; do not rewrite an existing
section. Current state and next actions live in
[VULKAN_PROBE_ACTIVE.md](VULKAN_PROBE_ACTIVE.md).

Phase A is the foundation gate of the Vulkan roadmap in
[VULKAN_PROBE_PLAN.md](VULKAN_PROBE_PLAN.md). Vulkan applications hand the
driver SPIR-V at pipeline creation, so the console itself must compile SPIR-V
into AGC shader packages.

| Step | What | Status |
|---|---|---|
| A1 | Build Mesa util and the NIR/ACO compiler (libpsbc) for the PS5 | done: built with this repository's payload SDK, and a console-title link resolves every symbol |
| A2 | A C/C++ AGC shader package writer | done: ps5-opengl's C writer reproduces every probe package |
| A3 | Compile SPIR-V on the console, draw the M2 triangle with an exact readback, and check the package is byte-identical to the PC-compiled one | done: all 14 probe packages compiled on the console byte for byte, and all 7 runner tests pass drawing with them |
| A4 | Gate: fall back to pipelines precompiled on the PC if A3 fails | not needed: A3 passed, so pipelines compile on the console |

This log records progress and findings in order. Hardware findings also go
into VULKAN_PROBE_PLAN.md.

## 2026-09-14: survey of the ps5-opengl SDK

ps5-opengl already compiles shaders on the console, so A1 and A2 start from a
working implementation rather than from scratch. Both projects are
GPL-3.0-or-later.

- Runtime compilation. ps5-opengl's Gallium driver
  (`src/gallium/ps5/ps5_screen.c`) compiles shaders at runtime on the PS5
  through `libpsbc.ps5.a`. It packages each result with
  `ps5_agc_package_build` (`src/platform/ps5_agc_package.c`).
- PS5 compiler build.
  - `toolchain/build-opengnm-psbc-ps5.sh` runs
    `toolchain/Makefile.opengnm-psbc-ps5` inside `third_party/opengnm-psbc`,
    with the flags in `toolchain/opengnm-psbc-ps5.mak`.
  - Compilers: `$PS5_PAYLOAD_SDK/bin/prospero-clang` and `prospero-clang++`,
    with `-std=gnu11` and `-std=c++17`, `-O2 -g -fPIC`, and
    `-DOPENGNM_PSBC_ORBIS=1`.
  - The archive holds the libpsbc front end, NIR, SPIR-V to NIR, ACO, AMD
    common and common NIR, the RADV shader sources (`radv_shader*.c`,
    `radv_postprocess_nir_standalone.c` and RADV NIR passes), Mesa util and
    util/format, compiler common, BLAKE3 (portable only), the C11 threads
    implementation over POSIX, two Vulkan runtime NIR passes, and
    `src/platform/ps5_mesa_shims.c`.
  - Mesa util sources left out on the PS5: `anon_file.c`, `log.c`,
    `mesa_cache_db.c`, `mesa_cache_db_multipart.c`, `os_file.c`,
    `os_time.c`, `u_process.c` and `u_thread.c`. Also `u_format_s3tc.c`.
  - `futex.c` builds with `-D__ORBIS__`.
- Gaps in the payload SDK's libc, filled by `ps5_mesa_shims.c`:
  - `open_memstream` is declared but not exported, and neither are
    `funopen`, `fopencookie`, `fmemopen` or `tmpfile`. The shim returns
    `ENOSYS`; ACO only uses a memory stream for optional diagnostics.
  - There is no CPU-affinity API. `util_set_thread_affinity` is a weak stub
    that reports failure.
- Pinned sources (SDK `dependencies.json`): opengnm-psbc
  `a92a1228ea3a64e4be9f0e61c2a65a5aa7ffed92` with
  `toolchain/opengnm-psbc-ps5.patch` applied, opengnm `4b295ca5`,
  SPIRV-Headers `0d25db97`, Vulkan-Headers `b51f6b86`.
  `tools/fetch-sources.py --verify-psbc` checks the patched tree hash offline.
  The SDK checkout already holds the patched tree and a host `libpsbc.a`, but
  no `libpsbc.ps5.a`.
- Compiler API (`libpsbc/psbc_compile.h`, metadata version 8):
  `psbc_init`, `psbc_compile_shader(spirv, size, options, output)`,
  `psbc_free_output`, `psbc_result_string`. `PsbcCompileOptions` carries every
  option our probe packages use: target PS5, stage, `ngg`, `address32_hi`,
  vertex attributes, descriptor bindings and `spi_shader_col_format`. The
  output holds raw ACO machine code and typed metadata: registers, linkage,
  user-data dwords, bindings and NGG LDS layout.
- C package writer (`ps5_agc_package_build(output, esgs_ring_itemsize,
  &package, &size)`): an ELF64 for machine 224 (AMD GPU) with
  `.shader_text`, a `.shader_header` (magic `0x34333231`, shader and context
  register tables, linkage, semantics and resource layout) and `.shstrtab`.
  It forces ESGS ring item size 1 for NGG vertex shaders, which our probe
  packages already use. It accepts unresolved program checksums, which
  hardware ran in M2-M4.
- Toolchain match. This repository's `.deps/native/ps5-payload-sdk/bin`
  provides `prospero-clang`, `prospero-clang++` and `prospero-ar`, as the
  SDK's PS5 makefile expects.

Plan from here:
1. A1: build `libpsbc.ps5.a` with this repository's payload SDK. The build
   runs in a private copy of the SDK's compiler tree under the build cache,
   leaving the SDK checkout untouched.
2. A2 precheck, PC only: compile the probe shaders with the same options,
   package them with ps5-opengl's C writer, and compare with the packages
   hardware already ran (written by the Python writer). If they match, the
   console's compiler output in A3 can be judged byte for byte against
   files known to work.
3. A3: a runner test that compiles, packages, links and draws on the console.

## 2026-09-14: A2, the C package writer matches the hardware-run packages

Every probe package the console has run was written by ps5-opengl's Python
writer (`tools/agc_shader_package_writer.py`). A console-side compiler would
use its C writer (`src/platform/ps5_agc_package.c`) instead. So A2 first asked
whether the two writers agree.

- `tools/build-psbc-cli.sh` gives the probe compiler `--agc-package FILE`.
  After compiling, it calls `ps5_agc_package_build(output, 1, ...)` and writes
  the package. The SDK's `ps5_agc_package.c` is compiled with the SDK's host
  flags and linked with the host `libpsbc.a`.
- `tools/build-probe-shaders.sh` now compiles both stages a second time
  through that option. It fails unless each C-written package equals the
  Python-written one byte for byte.
- Result: all six sets (`m2`, `m3-uniform`, `m3-vertex`, `m3-texture`,
  `m4-depth` and `m4-blend`) passed. That is 12 of 12 packages identical
  between the writers, including the M2-M4 packages hardware ran.
  Regenerating every set reproduced all committed package, checksum, binding
  and SHA256SUMS files. The only change is `m4-blend/PROVENANCE.txt`, which
  records the probe compiler's new sha256 (`97d79033`).
- The probe compiler's output without `--color-format` still equals the SDK
  compiler's (`m3-texture` and `m4-depth` pixel code and metadata identical).

## 2026-09-14: A1, libpsbc.ps5.a built with this repository's payload SDK

`tools/build-psbc-ps5.sh`:
1. Verifies the SDK's pinned, patched compiler tree
   (`fetch-sources.py --verify-psbc`: "exact pinned source tree verified").
2. Copies the compiler tree, opengnm, Vulkan-Headers and SPIRV-Headers
   includes, the PS5 make configuration and the shims into the ignored work
   directory `.deps/work/psbc-ps5` (`PSBC_PS5_WORK` overrides it). Timestamps
   are kept, so a rerun only recompiles what changed.
3. Applies one checked source patch to the work copy (`u_cpu_detect.c`,
   triage below). The SDK checkout is never modified.
4. Runs the SDK's `Makefile.opengnm-psbc-ps5` there with
   `PS5_PAYLOAD_SDK=.deps/native/ps5-payload-sdk`, for `libpsbc.ps5.a` plus
   the two objects of `libpsbc_support.ps5.a` (link check below).
5. Installs both archives with debug information stripped, `psbc_compile.h`
   and `PROVENANCE.txt` into `.deps/native/psbc`.

Result of a clean build:
- 472 archive objects and 2 support objects, built in 371 s (the first build
  took 301 s) with Ubuntu clang 18.1.3 targeting `x86_64-sie-ps5`.
- Reproducible: the clean build's archive has the same sha256 (`9690d6b3`)
  as the incremental build before it.
- Build archive: 130,334,146 bytes with debug information.
- Installed `libpsbc.ps5.a`: 30,128,866 bytes, sha256 `2ea2e2e4`. Installed
  `libpsbc_support.ps5.a`: 67,200 bytes, sha256 `ddaeb206`.
- 262 compiler warnings, all in upstream Mesa and opengnm sources and none in
  the support objects:
  - 232 `-Wunused-but-set-variable`, nearly all on a variable named `_`
  - 19 `-Wreturn-type`, mostly `spirv_to_nir.c` and `vtn_*.c`
  - 8 `-Wsometimes-uninitialized`
  - 2 `-Wvisibility` (`radv_instance.h`)
  - 1 `-Wconstant-conversion` (`ac_nir_meta_cs_clear_copy_buffer.c:264`)
- The first, unpatched build had 264 warnings. The difference is the two
  `-Wincompatible-pointer-types` warnings the patch removes.

One first attempt failed before compiling: `rsync` cannot create missing
parent directories, fixed by creating them.

### Triage: `util_cpu_detect` passes an `int` where `sysctl` writes a `size_t`

- The payload SDK defines neither `_SC_NPROCESSORS_ONLN` nor
  `_SC_NPROCESSORS_CONF`. `util_cpu_detect` therefore compiles its BSD
  `HW_NCPU` fallback for both CPU counts (`u_cpu_detect.c:819-827` and
  `840-848`).
- That fallback declares `int len` and passes `&len` as `sysctl`'s
  `size_t *oldlenp`. `sysctl` reads 8 bytes from a 4-byte variable, so the
  length's upper half is stack garbage, and writes 8 bytes back over the
  neighbouring stack slot. The `HW_NCPUONLINE` branch just above already
  uses `size_t`.
- ps5-opengl builds this code unpatched. Whether the overwrite hits anything
  depends on the stack layout the compiler chooses, so it can stay silent.
- Fix: `build-psbc-ps5.sh` changes both declarations to `size_t len` in the
  mirror copy. It stops if the upstream line no longer appears exactly twice.
  Both warnings are gone and every other warning is unchanged.

### Link check: what a console title needs beyond the archive

`tools/check-psbc-link.sh` compiles a small program that calls `psbc_init`,
`psbc_compile_shader`, `ps5_agc_package_build`, `psbc_free_output` and
`psbc_shutdown`. It links the program the way the console titles link
(this repository's CRT and C++ runtime objects, linker script, version
script and the payload SDK's system library stubs), then runs the title
converter on the result. The program is never packaged or run.

How it got to a pass:
1. The archive alone: lld listed 20 undefined symbols and stopped, which is
   its default error limit. The check now passes `--error-limit=0`: 56
   symbols.
2. The 56 symbols, and what supplies each:

| Missing | Why | Supplied by |
|---|---|---|
| 48 `util_format_dxt{1,3,5}_*` pack, unpack and fetch functions | ps5-opengl leaves `u_format_s3tc.c` out because its own Mesa build provides it, but the format table still references them | upstream `u_format_s3tc.c`, compiled into `libpsbc_support.ps5.a` |
| `__assert` | NIR keeps assertions (the C sources build without `NDEBUG`), and the console runtime stubs have no FreeBSD assert hook | `tooling/psbc/psbc_ps5_shims.c`: report, then abort |
| `popen`, `pclose` | ACO's optional disassembly (`aco::print_asm`) runs objdump | shims: fail with `ENOSYS` |
| `_mesa_log_multiline` | NIR's annotated shader printing; Mesa's `log.c` is left out | shims: one `tag: level: line` per line on stderr, as upstream's file logger writes. `log.c` itself would pull in `u_process.c`, syslog and file logging |
| `__emutls_get_address` | emulated TLS (`-femulated-tls`), used by `u_qsort`'s thread-local comparator | Clang's builtins archive (`emutls.c.o`) |
| `std::__next_prime`, `std::logic_error` and `std::bad_array_new_length` constructors | ACO's unordered containers and libc++'s throw helpers | the payload SDK's `libc++.a` and `libc++abi.a` |

3. libc++abi and libunwind name `pthread` as a dependent library, so the link
   searches the payload SDK's `target/lib`.
4. libunwind finds unwind tables through `__eh_frame_start`/`_end` and
   `__eh_frame_hdr_start`/`_end`. `tooling/psbc/ps5-pie-unwind.ld` includes the
   titles' layout unchanged and provides those four symbols, as ps5-opengl's
   `native-app/ps5-pie.ld` does.

Result: PASS.
- lld resolves every symbol and writes a 15,669,864-byte ELF.
  `psbc_compile_shader` reaches nearly the whole compiler.
- Static runtime members linked, recorded with `--why-extract`:
  - libc++: `hash`, `new_helpers`, `stdexcept`
  - libc++abi: 13 members (exception throwing and personality, demangler,
    type information, fallback allocator)
  - libunwind: 4 members
  - Clang builtins: `emutls` only
- The title converter accepted all 125 imports. They are ordinary libc,
  libm and pthread functions, including:
  - condition variables, mutexes (including `pthread_mutex_timedlock`),
    read-write locks, keys, `pthread_once`, `create` and `join`
  - `setjmp`/`longjmp`, `sysctl`, `sysconf`, `getrlimit`, `mkstemp`,
    `system`
  - `getenv`/`setenv`/`unsetenv`, `aligned_alloc` and `posix_memalign`
  - math functions

Consequences for A3:
- The runner's link recipe:
  - `libpsbc.ps5.a`, `libpsbc_support.ps5.a`, the SDK's `libc++.a`,
    `libc++abi.a` and `libunwind.a`, and Clang's builtins, in one group
  - `-L` the payload SDK's `target/lib`
  - `tooling/psbc/ps5-pie-unwind.ld`

  ACO keeps C++ exceptions enabled; our own sources keep `-fno-exceptions`.
- `NOTICE.md` says titles do not link the complete libc++ or libc++abi. The
  runner will statically link the members listed above (Apache-2.0 WITH
  LLVM-exception, compatible with GPL-3.0), so `NOTICE.md` changes with A3.
- The converter only checks imports against the SDK stubs' names. A3's first
  launch confirms that the console exports each one.
- The runner's executable grows by roughly the probe ELF's size, about 15 MB.
- The same library calls on the console (`psbc_compile_shader`, then
  `ps5_agc_package_build`) must produce packages byte-identical to
  `probes/<set>/*.bin`, so the console's compiler output can be checked
  exactly.

## 2026-09-14: A3, the console compiles every probe shader byte for byte

A3 asked whether the PS5 itself can turn SPIR-V into AGC shader packages that
equal the PC-compiled packages hardware already ran, and draw with them.
Rather than a single M2 test, compilation became a runner mode: every
existing test can run on console-compiled shaders, with no duplicated drawing
code.

### What was built

- Probe sets (`tools/build-probe-shaders.sh`): every set now ships
  `vertex.spv`, `pixel.spv` and `compile.txt`, the exact compiler options in
  opengnm-psbc CLI syntax, and `SHA256SUMS` covers all three. Regenerating all
  six sets reproduced every committed package, checksum, binding and
  provenance file; only each `SHA256SUMS` gained the three new entries.
- Runner queue keyword `compile` (`src/diagnostics.cpp`, runner built with
  `AGC_SHADER_COMPILER=1`). `psbc_init` runs once for the queue. For each
  queued test and each stage, `compile_shader_package`:
  1. reads the stage's line of `compile.txt` and parses it the way
     `cmd/psbc/main.c` parses its arguments, with the same defaults and field
     formats, plus the probe CLI's `--color-format`; unknown or malformed
     options reject the line
  2. reads `<stage>.spv`, which must be a whole number of words
  3. compiles it with `psbc_compile_shader`, and packages the result with
     `ps5_agc_package_build` at ESGS ring item size 1
  4. compares the package with `<stage>.bin` and logs
     `agc_console_compile_<stage>`: SPIR-V and package sizes, the compile
     time, the FNV-1a 64, and on a mismatch the first differing byte and the
     number of differing bytes
  5. replaces the loaded package with the compiled one, so validation,
     `checksums.txt`, shader creation, linking and drawing all use the
     console's output. A mismatch skips shader creation.
- Build:
  - `tools/psbc-link.sh` holds the compiler link recipe proved in A1.
    `tools/build.sh` uses it for `AGC_SHADER_COMPILER=1`, and
    `tools/check-psbc-link.sh` uses the same recipe.
  - ps5-opengl's C writer moved into `libpsbc_support.ps5.a`, with its header
    installed beside `psbc_compile.h`. Titles need nothing from the SDK
    checkout.
  - The runner's executable is 15,182,997 bytes (15,760,456-byte ELF before
    signing), with 185 imports, all accepted by the title converter.
  - All five titles still build without warnings.
- PC: the host runner links the SDK's host `libpsbc.a` and the C writer
  object, so `python3 tools/golden.py rebuild golden/runner --compile` runs
  the same compile mode.
- `jobs/compile/queue.txt`: `compile`, `hold 60`, `all`.

### Results on the PC

- `golden.py rebuild --compile`: 10 of 10 golden frames rebuilt identically
  from packages compiled by the host runner, with every compile
  byte-identical.
- `golden.py rebuild` without compile mode: 10 of 10 identical, so nothing
  else changed.
- The PC runner completes frames without a GPU, so its pixel readbacks never
  pass (`runner_test` is `FAIL` on the PC in both modes). The PC checks
  therefore judge command streams, compile events and linking, not pixels.
- Negative tests on the PC runner, each on a scratch copy of `probes/`:

| Case | Change | Result |
|---|---|---|
| control | none | both compiles `PASS`, shaders link |
| option changed | `m2` pixel line gains `--color-format 0x99999994` | pixel `FAIL` "differs from the PC-compiled package": 856 instead of 864 bytes, first difference at byte 40, 158 bytes differ; shader creation skipped |
| unknown option | `m2` vertex line gains `--bogus` | vertex `FAIL` "missing or malformed compile.txt line"; shader creation skipped |
| truncated SPIR-V | `m2/vertex.spv` one byte short | vertex `FAIL` "missing SPIR-V, or not a whole number of words"; shader creation skipped |

  A first attempt changed `--address32-hi` instead. It produced the same
  package, because the `m2` pixel shader reads no GPU pointers; that option
  is not a valid fault for `m2`.

### Result on the console

Runner pid 290, `Klog_Logs/klog-20260914-231534.log`:
- All 7 tests passed, with their unchanged exact readbacks, drawing with
  console-compiled packages: `m2-solid` (the A3 gate), `m3-uniform`,
  `m3-vertex`, `m3-texture`, `m4-depth`, `m4-blend` and `m4-rtt`.
- All 14 compiled packages were byte-identical to the PC-compiled packages.
  The sets cover vertex attributes, a uniform buffer, a combined image
  sampler, 32-bit GPU pointers and FP16_ABGR colour exports.
- Measured compile time, NIR through ACO plus packaging and excluding
  `psbc_init`: vertex shaders 1,043 to 1,139 µs, pixel shaders 308 to 469 µs
  (`clock_gettime(CLOCK_MONOTONIC)`, not cross-checked against another
  clock).
- Probe statuses: 398 PASS, 50 INFO, 28 NOT_REQUIRED, 9 ARMED, and 7 WARN,
  the known benign VideoOut busy result on unregister, once per test.

### Findings

- The title with the compiler loaded and ran, so the console exports every
  import the compiler needs (the open question from A1's link check). The
  system allocator was enough for these shaders; no private heap like
  ps5-opengl's was needed.
- Compilation on the console is deterministic and equals the PC: a
  different C library, allocator and address space produced the same bytes,
  so nothing in NIR or ACO depends on pointer order for these shaders.
- A4's fallback to PC-precompiled pipelines is not needed. Phase A is
  complete, and Phase B can create pipelines from SPIR-V on the console.
