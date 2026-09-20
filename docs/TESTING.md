# Testing

The repository separates fast host checks from behavior that only real PS5
hardware can prove.

## Commands

| Command | Scope |
| --- | --- |
| `make test-deps` | Fetch and verify the pinned host-only GoogleTest source. |
| `make test-unit` | Compile and run the host-native GoogleTest application tests. |
| `make test-integration` | Exercise repository scripts through subprocesses and temporary files. |
| `make test` | Run both host test suites. |
| `make check` | Run linting, all host tests, and a complete folder build. |
| `tools/check-fragment-inputs.sh` | Compile dense and sparse pairs of fragment varyings with the driver compiler work copy; require distinct resolved AGC input semantics. Included in `tools/check-driver.sh`. |
| `tools/check-runner-cases.sh` | Run the runner's driver cases on the PC through the driver-enabled host runner and require its summary to pass. |
| `tools/command_audit.py --check` | Every Vulkan 1.0 command, and every command of an exposed extension, is implemented, refused by name with the phase that would implement it, or supplied by the runtime (a gate in `make test`). |

GitHub Actions runs `make test-unit` and `make test-integration` as separate
steps on its Ubuntu runner, so every pull request executes both layers with
clear failure reporting. The local development host is CachyOS; the CI runner
is not a supported build host and exists only to gate pull requests. The runner
installs the LLVM 18 packages from the Ubuntu archive and links those binaries
over the unversioned names the scripts resolve first, so a second Clang in the
runner image cannot serve the gates. The supported host ships a newer LLVM; the
shared format policy is verified against both, which is why `make lint` passes
on either.

`make ci` runs the workflow's build job on this host, in the same order, with
the same identity check, runtime digests, compressed image, ZIP, and checksum
manifest. Only the runner's package installation and the release-tag rule are
not reproduced. A cold `.deps/` cache makes the first run fetch the pinned
dependencies, exactly as the runner does. Host tests must remain
deterministic, must never contact a console, and must be safe to run in
parallel with unrelated console work. The first unit-test run downloads a
pinned GoogleTest archive after verifying its SHA-256; later runs reuse
`.deps/test/`.

Run one test or suite with normal GoogleTest arguments:

```bash
make test-unit GTEST_ARGS='--gtest_filter=AssetTextTest.MissingAssetUsesFallback'
```

## Unit-test policy

Write unit tests for reusable logic with meaningful behavior: parsers, state
transitions, bounds handling, input mapping, protocol messages, resource
ownership, and error paths. Keep platform calls behind a small boundary so the
logic can compile and run on Linux without a PS5 or proprietary SDK.

The starter suite in `tests/test_demo_renderer.cpp` uses GoogleTest to validate
fallback, line-ending, truncation, and null-termination behavior for packaged
text assets. GoogleTest is a host-only development dependency: it is never
compiled into `eboot.bin`, `libc.prx`, or a PS5 package.

Do not add tests for trivial constants or one-line drawing calls merely to
increase a coverage percentage. Test observable contracts and regressions.

## Host integration tests

`tests/test_tools.py` invokes complete repository scripts with temporary input
and controlled environment variables. Use this level for metadata updates,
build orchestration, package validation, and deployment resolution. Network
operations must be mocked or use an explicit dry-run mode; host CI must never
contact a console.

Each test must clean up its files, avoid shared mutable state, and include the
failure case that would have caught the associated bug.

## PS5 integration validation

Rendering, controller input, AudioOut, mounted paths, launch/closure behavior,
and firmware compatibility require hardware validation. A passing host suite
does not prove those properties.

For a hardware milestone:

1. Build an exact candidate from a clean commit and record its digest.
2. Acquire the shared console lock only for the test window.
3. Deploy the title through the documented LAN-only procedure.
4. Capture the expected visual result and relevant logs.
5. Close the title, release the lock, and record firmware, loader, result, and
   artifact identity.
6. Commit the validation record separately from the implementation when the
   project workflow requires one.

Follow [Deployment](DEPLOYMENT.md) and the separate
[PS5 Homebrew Development Protocol](https://github.com/blackbearreloaded/ps5-homebrew-dev-protocol)
for console coordination, evidence collection, and milestone policy.

## Adding tests

- Add GoogleTest cases to C++ files under `tests/`; the `test-unit` recipe owns
  their host-only compilation.
- Add Python subprocess tests as `tests/test_*.py`; discovery is automatic.
- Preserve the GPL header on every test source.
- Run `make test`, `make lint`, and `make` before submitting a change.
- Reserve real-console claims for recorded hardware results.
- A runner case must not allocate tens of megabytes on the heap: the title's heap
  does not promise them and the runner is built without exceptions, so a failed
  allocation aborts the title with no record at all. Read the harness's mapped
  memory instead, or fold it into a checksum (Phase C8's resolve case did the
  first and now does the second, docs/M5_PHASE_B.md).
- When proving a guard is sensitive, remove `tests/__pycache__` (or run
  `python3 -B`): a same-size edit to a test file can leave Python running the
  cached bytecode, which makes a broken guard look green.
- An audit that cannot be green yet is gated on its **record**: the required
  limit table reports 0 missing and is checked directly, while the format audit
  still has 55 gaps and is checked against `docs/V0_FORMATS_AUDIT.md` -- the four
  counts and the row-by-row gap list (`tests/test_tools.py`, `FormatAuditTests`),
  so a driver change that closes or opens a gap has to move the document with it.
- A **runner case that goes through the Vulkan driver** can be run and read on
  the PC before the console runs it. `bash tools/build-host-runner.sh --driver`
  builds `build/host/runner_host_driver`, which links the driver, Mesa's runtime
  and the B7 program `driver/tests/ps5vk_triangle.c` the way the console title
  does (`tools/build.sh`, `AGC_VULKAN_DRIVER=1`); run it with

  ```bash
  build/host/runner_host_driver --replay REPLAY --memory free \
      --app0 . --download0 DIR --queue FILE
  ```

  `--memory free` is the part a driver case needs: the replay then supplies the
  register defaults and the pipeline stage images alone, because the driver maps
  its own memory exactly as it does on the console, while an AGC-level frame has
  to land on the addresses its capture names (`ps5_host_load_replay`). The
  replay must come from a golden a **driver** run captured
  (`python3 tools/golden.py replay golden/<driver case>/run-1.json`), or the
  driver's own 2 MiB queue buffer has no captured region to map. `--queue` is a
  file with one case name per line; `capture` first when the frames should be
  recorded. The AGC-level runner (`tools/build-host-runner.sh`, no flag) is what
  `tools/golden.py rebuild` uses, and it does not contain the driver cases.

  A driver case that only queries or submits **works** this way (`v0-formats`,
  `c2-transfers`, `c7-clear` and `b5-events` are run so far, and
  `tools/check-runner-cases.sh` is the recipe as a gate: it builds what is
  missing, writes the queues and the replays, requires the runner's summary to
  pass for the cases that draw nothing and compares the submission a drawing case
  dumps with the console's own frame -- `tests/test_tools.py` checks that every
  case either list names is one the runner's table holds).

  Add `--cases driver` when **every** queued case goes through the driver. Such a
  case brings its own device and shaders (`ps5vk_triangle_create`,
  `vk_icdGetInstanceProcAddr`), so the runner skips the AGC-level package staging
  -- and it has to: that staging's 64 KiB stage workspace is allocated before the
  case runs, and the host layer hands a captured region to the **first allocation
  of its size**, so the workspace would take the captured pipeline stage the
  driver's own stage allocation needs. The driver's stage then lands in free
  memory (0x20000c000 against 0x200038000), its linked output keeps the
  compiler's unrelocated offsets (0x78 and 0x40 where the passing driver test
  reads 0x200038090 and 0x200038060), and `ps5vk_CreateGraphicsPipelines` refuses
  it with "a created shader's register tables are out of bounds"
  (docs/M5_PHASE_B.md has the trace).

  A drawing driver case then **runs** -- its pipeline is created, its command
  buffer records and its submission is written -- but its *frame* is empty on the
  PC, because nothing renders there: the frame is what the console run proves.
  What the PC can prove is the submission, with the same comparison
  `tools/check-driver.sh` uses:

  ```bash
  PS5_HOST_SUBMISSION_DUMP=dump.json build/host/runner_host_driver \
      --replay REPLAY --memory free --cases driver --app0 . --download0 DIR \
      --queue QUEUE
  python3 tools/golden.py compare-submission golden/b5/b4-headless-1.json dump.json \
      --draws 1 --extra-sh-register 0x8c --extra-sh-register 0x0c \
      --expect-record 0x111=0x44870000
  ```

  `c2-indirect` and `b8-secondary` are checked that way: both submissions are
  identical to the console's b4-headless frame (6 packets, 3 register tables).
