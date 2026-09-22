# R12 padded texture row pitch — unverified, not deployed

The port's PID 195 (build 6b437103…) reached a named refusal for a 32-wide
sampled image with 256-byte stored rows. The candidate follows the local
OpenGL driver's single-level 2D encoding: descriptor word 4 = pitch texels - 1.
The patch adds c4-padded, reusing the existing full-frame nearest/bilinear
readback at width 32. The queue includes m2-solid for same-run context defaults.

**Correction:** the first `bash tools/check-driver.sh c4_texture` result was
incorrectly reported as testing this change. That script does not rebuild the
driver: its three passing arms used the R11 archive. `tools/build.sh` caught
that archive as older than the source. A fresh `bash tools/build-driver.sh`
then failed in all three modes because `ALIGN` is not declared in this source.
There is no successful R12 build, host validation or console evidence. The
mission's stop-on-contradiction rule ends this cycle here. The candidate was
removed from the working sources, preserving it in this patch.

Resume plan:
1. Apply `git apply parked/r12-row-pitch/row-pitch.patch`; copy queue.txt to
   jobs/r12-pitch/queue.txt. Replace the unsupported ALIGN macro with the
   existing Mesa alignment helper after checking its declaration.
2. Build explicitly with `bash tools/build-driver.sh`, then run
   `bash tools/check-driver.sh` and `bash build/gates.sh`. Inspect every gate's
   result; the local gate aggregator does not propagate failures.
3. Build PPSA99988 with the runner defines, check the port's shader scan, verify
   console idle, deploy and read back its eboot before running this queue once.
4. Require exact nearest pixels and existing-tolerance bilinear pixels for both
   c4-texture and c4-padded. Capture new goldens, replay with this run's defaults.
5. Only after hardware proof, commit in the driver, rebuild/relink the port,
   verify all port gates and run one new identity. Keep the template regression
   check. No M2 or M6 claim without the required human/hardware acceptance.

Raw local diagnostics: build/r12-host-texture.log (baseline PASS),
build/r12-runner-build.log (stale archive guard), build/r12-build-driver.log
(fresh compile failure). No console address or credentials are stored here.
