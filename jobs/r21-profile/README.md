# R21: measure vkQuake's queue cost

Opt-in queue timing is enabled by the presence of /app0/ps5vk-profile.txt, or
by setting PS5VK_PROFILE in a host process. It changes no command words or
synchronization. The first successful present discards startup work; later
reports cover approximately ten seconds each.

Each report contains successful frames, submission steps per frame, and
per-frame wall-clock averages:

- queue_ms: queue waits, copies, GPU submissions and signaling together.
- flush_ms: target CPU cache eviction, included in queue_ms.
- gpu_ms: native submission, suspend point and completion-marker polling,
  also included in queue_ms; this is not an isolated GPU shader timer.
- flip_ms: native flip submission and waiting for its presentation marker.
- flush_MiB/frame: target bytes passed to CPU cache eviction per frame.

These overlapping measurements must not be added together. Time outside
queue and flip includes application recording, simulation and frame pacing.
The instrumentation is disabled on normal launches without either opt-in.

Verification: explicitly rebuild with tools/build-driver.sh, run
PS5VK_SHADER_CACHE_DIR="$PWD/build/host-regression-cache" tools/check-driver.sh,
and all eleven repository gates. Relink both consumers. Deploy the port,
verify all deployed ELF load segments, run its shader scan, then launch with
the title-local flag and kernel capture. Read final traces twice and match
identity and PID. Remove the flag while idle when measurement is finished.

The R20 subpass stream also replays exactly with PS5VK_PROFILE=1: the console
captures frame zero's two submissions; the host's full dump retains both
frames. This proves the opt-in path leaves those command packets unchanged.
See replay.txt. Console timing results are recorded in baseline.txt.
