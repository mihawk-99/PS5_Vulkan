# Deployment

This project creates a directory-style homebrew application and optional
filesystem images. Tagged releases also provide the complete directory as a
ZIP archive. Its Makefile can update the directory or upload an image below
`/data/homebrew` over FTP. It does not configure the console, register titles,
or create a signed retail package. The one payload it ships,
`build/ps5vkctl/ps5vkctl.elf`, starts and closes the title the console has
already been set up to run (see "Autonomous run loop" below); it changes nothing
about the console's configuration.

## Requirements

Use a console you own with an already configured, compatible homebrew
environment and loader. Follow that loader's documentation for setup and
supported input formats. This repository does not configure the console.

Keep loader, mount, and FTP services on a trusted local network. The automated
path uses Python 3's standard-library FTP client and requires an already-running
FTP service on the console.

## One-command development deployment

Build and update the title folder through the default FTP port `2121`:

```bash
make deploy PS5_HOST=192.168.1.100
```

The default folder deployment:

1. builds `dist/<TITLE_ID>/` from the current source;
2. uploads each file beside its destination under a hidden `.upload` name;
3. replaces that file only after its transfer completes;
4. publishes `eboot.bin` and then `sce_sys/param.json` last; and
5. verifies that both required files appear in the remote directory.

The FTP client uses current-directory `MLSD` checks and accepts any successful
2xx completion for file deletion. This accommodates the homebrew `ftpsrv`
behavior used by the validated 6.02 and 12.70 environments while preserving
the temporary-upload and required-file gates.

Keeping `/data/homebrew/<TITLE_ID>/` itself in place preserves ShadowMountPlus's
existing nullfs source while updating what the next launch reads. Fully close
the application before deploying and do not launch it until the command
finishes. Files removed from the local build are not deleted remotely; clean
the title directory with `make undeploy` when an exact reset is required.

Select an image or a non-default port with Make variables:

```bash
make deploy PS5_HOST=192.168.1.100 DEPLOY_FORMAT=ffpfsc
make deploy PS5_HOST=192.168.1.100 FTP_PORT=2121 DEPLOY_FORMAT=ffpkg
```

Image deployment remains useful for distribution testing. An already-mounted
image with the same pathname may remain cached by ShadowMountPlus, so folder
deployment is the recommended repeated development workflow. Do not keep a
folder and an image with the same title ID in scan paths at the same time.

Supported variables are:

| Variable | Default | Purpose |
| --- | --- | --- |
| `PS5_HOST` | required | Console IPv4 address or hostname |
| `FTP_PORT` | `2121` | FTP service port |
| `DEPLOY_FORMAT` | `folder` | `folder`, `ffpfsc`, or `ffpkg` output |
| `PS5_FTP_USER` | `anonymous` | FTP username |
| `PS5_FTP_PASSWORD` | `codex` | FTP password |
| `DEPLOY_DRY_RUN` | `0` | Use `1` to build and print the target without networking |

For repeated local work, copy `.env.example` to the ignored `.env` file and
set `PS5_HOST`, `FTP_PORT`, and `DEPLOY_FORMAT` there. Command-line Make values
still override file defaults.

For example, validate local packaging and the resolved destination without
contacting a console:

```bash
make deploy PS5_HOST=192.0.2.1 DEPLOY_DRY_RUN=1
```

## Remove the staged development copy

Fully close the application, then remove the current `titleId` from the FTP
staging area:

```bash
make undeploy PS5_HOST=192.168.1.100
```

The command validates `sce_sys/param.json`, recursively removes only
`/data/homebrew/<TITLE_ID>/`, and deletes exact same-ID `.ffpkg` and `.ffpfsc`
files plus interrupted-upload temporary images. It never deletes the
`/data/homebrew` root or another title. Preview the resolved targets without a
network request by adding `DEPLOY_DRY_RUN=1`.

This is deliberately named **undeploy**, not uninstall: FTP removal does not
unregister the title from the PS5 Shell database. A stale home-screen entry may
remain until the loader refreshes or dedicated, separately authorized cleanup
tooling unregisters it. The command fails if the FTP server cannot enumerate a
directory safely or if an active mount prevents removal.

## Recommended edit-test loop

1. Fully close the previous application and any remaining crash dialog.
2. Confirm the console's FTP and mount services are ready.
3. Edit the source. Update `contentVersion` in `sce_sys/param.json` for a
   release-worthy change; routine folder deployments do not require a bump.
4. Run `make deploy PS5_HOST=<console-address>`.
5. Wait for the mount service to report the title ready, then launch it
   manually.
6. Observe the result and fully close the application before deploying again.

Keeping launch and close actions manual makes the default command predictable
and avoids replacing a package while its previous title remains active. With the
control payload loaded, one command does steps 5 and 6 as well: see "Autonomous
run loop" below.

A title that links the Vulkan driver (`AGC_VULKAN_DRIVER=1`, the test runner)
links `build/driver/ps5/libps5vk.ps5.a`, which `make deploy` does not rebuild:
run `tools/build-driver.sh` after any change under `driver/` before deploying,
or the console runs the previous driver. `grep -c "<a string from the change>"
dist/<TITLE_ID>/eboot.bin` confirms what was shipped -- a stale archive cost one
render-to-texture console run (docs/M5_PHASE_C.md, 2026-09-17).

## Autonomous run loop

Every probe battery costs one launch of the runner title, and a title cannot
launch itself: the system refuses to launch an application that is already
running, and a title that a probe has just faulted the GPU inside cannot be
trusted to close itself. The console therefore keeps a small agent resident,
`payload/ps5vkctl`, and the repository's tools drive it.

1. Build it once with the PS5 payload SDK
   (<https://github.com/ps5-payload-dev/sdk>, release ZIP, unpacked anywhere and
   pointed at with `PS5_PAYLOAD_SDK`, or `~/ps5-payload-sdk`):

   ```bash
   tools/build-ps5vkctl.sh          # build/ps5vkctl/ps5vkctl.elf
   ```

2. Put it on the console and load it once per console boot with the console's
   own loader. `deploy-payload` uploads it to `/data/homebrew/ps5vkctl.elf` and
   to `/data/etaHEN/payloads/ps5vkctl.elf`, where payload menus list it:

   ```bash
   python3 tools/ps5_console.py deploy-payload
   python3 tools/ps5_console.py payload      # ok ps5vkctl 1 pid=<n> / ok idle
   ```

   The agent answers on port `9111` (`PS5VKCTL_PORT`), logs to
   `/data/ps5vkctl.log`, and speaks one command per connection: `ping`,
   `status`, `users`, `procs`, `launch <TITLEID>`, `kill <TITLEID>`,
   `restart <TITLEID>` and `quit`. `launch` tries the calls the ecosystem's
   dashboards use (`sceLncUtilLaunchApp` and `sceSystemServiceLaunchApp`, with
   the foreground and the logged-in user) and reports what each returned;
   `kill` suspends the title, SIGKILLs its processes and then calls
   `sceLncUtilKillApp`.

3. Queue a battery, arm the capture, restart the title and read the result in
   one command. The capture is armed before the launch, so no record of the run
   is missed:

   ```bash
   python3 tools/ps5_console.py battery PPSA99988 jobs/v0-query/queue.txt \
       --output Klog_Logs/v0-query-run2.log
   ```

   It exits `0` when the run ended and passed, `3` when the run did not end and
   `2` when no run arrived. Individual steps stay available as
   `push-jobs`, `klog`, `launch`, `kill` and `restart`.

   A queue that the runner would reject is caught on the PC before it costs a
   session: `tests/test_tools.py` (the queue guards) fails when a line names a
   case the runner's table does not hold and no parked patch adds, when a queue
   holds more than 32 tests, when its `hold` is outside 1..3600, or when it names
   no test and no `all`.

4. A queue that ends with `exit` leaves the console when its batch is done
   instead of drawing its banner forever, which is what lets the next deploy's
   `eboot.bin` be the one the next launch loads. A batch that faults the GPU
   takes the title down with it, so the loop survives that too. Only the runner
   honours `exit`; the default build still keeps `main` alive.

Safety: `kill` and `restart` name a title id and refuse to act unless the
console's running application really is that title, and `PPSA99999` -- the
default profile this project never launches for a run -- is refused outright.

## Manual build and stage

1. Build the exact format accepted by your loader:

   ```bash
   make          # directory form
   make ffpkg    # directory plus UFS2 image
   make ffpfsc   # directory plus compressed image
   ```

2. Choose one complete output supported by the loader:

   - `dist/<TITLE_ID>/`: directory form;
   - `dist/<TITLE_ID>.ffpkg`: UFS2 image;
   - `dist/<TITLE_ID>.ffpfsc`: compressed image.

3. For directory deployment, stage the entire `dist/<TITLE_ID>/` tree. Do not
   upload only `eboot.bin`.
4. Wait for the loader to report that the title is ready, then launch it from
   the Games section of the home screen.

Use `make packages` only when both optional image formats are needed. Rebuild
the selected format immediately before deployment so an older package is not
mistaken for the current application.

## Deploy a tagged-release ZIP

Tagged GitHub Releases include `<TITLE_ID>.zip` as a transport-friendly copy
of the validated directory-style application. Extract it locally; the archive
contains one top-level `<TITLE_ID>/` folder. Upload that complete folder so the
result is `/data/homebrew/<TITLE_ID>/eboot.bin` with its `sce_sys/`,
`sce_module/`, and asset directories beside it.

Do not upload the ZIP file itself and do not extract only its contents directly
into `/data/homebrew`. ShadowMountPlus consumes the extracted title folder, not
the ZIP container. The extracted folder and the `.ffpfsc` release asset contain
equivalent application content, so stage only one form for a given title ID.

## Smoke test

The default skeleton hides the splash screen, renders the Hello World frame,
loads `/app0/assets/banner.txt`, and remains alive. Close it through the
home-screen interface.

The skeleton intentionally keeps `main` alive. Do not return from `main` or
call an exit function unless the target loader and application lifecycle
explicitly support that path. The runner has exactly one such path: a job queue
whose last line is `exit` returns from `main` when its batch is done, which is
how the control payload's loop (above) replaces a previous build with the next
one. Every battery up to pid 158 ended with the runner still on screen; the
queue that first asks for the exit is `jobs/v0-query/queue.txt`.

If launch fails, see [Troubleshooting](TROUBLESHOOTING.md).
