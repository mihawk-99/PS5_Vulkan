# R29: common tile-address evaluation

Status: address-map correctness and gates pass; game performance retest is pending.
No application speedup is claimed before that measurement.

The existing measured one-sample, four-byte 128x128 tile equation has nine
terms and variable divisors. The candidate spells that same equation directly
in its shared address helper, eliminating interpretation and division for this
common case. Other layouts still take the original path. Mip-tail XOR remains
at the existing caller. No new layout, firmware data, or supported format.

Verification:

- `python3 jobs/r29-tile-address/check.py`: 2,441,216 addresses match the
  original helper extracted from commit 9f9f395; 144 random-colour reductions
  match an independent byte oracle, including row/tile combinations, offsets,
  tail XOR and untouched bytes.
- PS5 PID 267: 178 PASS, zero FAIL; original solid plus four complete 4K mip
  frames match all 8,294,400 pixels each. Each mip case also verifies all
  87,040 generated lower-level texels. Goldens: golden/r29-tile-address.
- `python3 jobs/r29-tile-address/replay.py`: four submissions compare exactly.
- PS5 PID 268: 3,501 PASS, zero FAIL across m2-solid, r16-mip-blit,
  c7-mip-upload, c7-copy and c7-blit-formats. The second battery disables verbose
  capture, so transfer dumps cannot consume the execution deadline.
- `python3 jobs/r29-tile-address/check-console.py` verifies the distilled
  per-pixel and completed-case evidence. Both titles were closed; idle verified.
- Explicit driver build, full host/cache suite, eleven gates, port five gates
  and shader scan, template relink pass. Served executable read twice; all five
  load segments match. archive.txt and deployed-proof.txt identify the build.

An earlier integer-only 2:1 filter is retained as integer-filter.patch, but is
not applied. PID 263's verbose capture timed out during c7-copy after completing
mip and upload cases; golden/r29-half-blit-capture intentionally retains that
partial run. PID 264 repeats the entire battery without capture: 3,501 PASS.
Ten completed mip/upload submissions replay exactly via replay-integer.py.
Game PID 265 reduced copy time from 24.095 to 22.657 ms, without improving FPS;
the first candidate was rolled back. Port m6-mip-integer-benchmark holds the run.

The game executable for the new candidate is built with identity e3525e30….
Deployment on 2026-09-23 failed before FTP connected: the PS5 stopped answering
FTP, control and klog, with a failed neighbor entry despite an available local
route. No new fixture or executable was uploaded. No game speedup is claimed.
The prepared benchmark repeats start/E1M1 and includes host_speeds samples;
movement, firing, all shareware maps and save/load are queued after it.
