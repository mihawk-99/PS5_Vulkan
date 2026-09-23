# Internal NIR shader caching — pending console validation

The game still compiles eight internal NIR stages on every launch. This small
candidate reuses Mesa's existing pointer-free, stripped NIR serialization as
the input to the existing persistent output cache. A zero prefix distinguishes
NIR from a valid SPIR-V module. Compiler options/build identity, checksums,
atomic stores and corruption fallback remain shared. No GPU addresses persist.

This is parked because the console stopped answering network services after
R29's successful probe runs. It is not in production source and has no measured
PS5 startup benefit yet. It was developed on c3e51f6, after the tiled address
change. Finish the already-built R29 game benchmark before relinking this.

Host validation already completed:

- Explicit driver rebuild, full loader/direct/PS5-link and cache suite PASS.
- Eleven gates PASS, recorded in gates.txt.
- check.py: NIR clone/debug-name changes preserve keys; semantic-info and
  compiler-option changes invalidate them. Separate cold/warm/disabled processes
  emit byte-identical compiler output. The warm process compiles nothing.
- replay.py: cold run compiles two NIR and two SPIR-V shaders; warm run compiles
  none and has four hits. Four mip submissions in each process compare exactly
  to the PS5 capture: eight comparisons, no changed word or tolerance.
- The ten earlier mip/upload submissions also replay exactly with the candidate.

Reproduce from the driver root:

```sh
git apply parked/nir-shader-cache/driver.patch
bash tools/build-driver.sh
python3 parked/nir-shader-cache/check.py
bash tools/build-host-runner.sh --driver
python3 parked/nir-shader-cache/replay.py
bash tools/check-driver.sh
bash build/gates.sh
```

Remaining acceptance:

1. Build the console probe and run mip/menu-alpha cases twice with this same
   binary. Verify cold stores, warm hits, zero warm NIR compilation, complete
   pixel readback, strict replay and executable/PID/idle proofs. Preserve caches.
2. Relink port/template, run all port gates and scan, then measure same-binary
   cold/warm game startup and run its normal map/menu/gameplay fixture. Verify
   actual readback, final traces twice, clean exit and no refusals/audio errors.
3. Record only measured startup benefit, restore test configs/profile, then land
   the production patch and remove this parked status. No physical confirmation
   is required for it.

The existing common cache tests cover unavailable directories and corrupt or
truncated output recovery. Cache-directory disable still compiles normally.

## Landed (R46)

Applied unchanged to production after the console acceptance above was run;
see `jobs/r46-nir-cache`. This directory stays as the record of how it was
developed.
