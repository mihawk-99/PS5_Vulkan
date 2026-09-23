# R26: sampler LOD bias used by vkQuake render scaling

Game PID 248 changes r_scale from 1 to 2 and creates a sampler with bias 1.
The driver advertised maxSamplerLodBias 2 but rejected every nonzero bias, so
vkQuake exited through its error handler. The failed application run remains in
../PS5_vkQuake/evidence/m6-scale-sampler-refusal.

The sampler now encodes signed 8-fraction-bit bias in its existing word 2 field,
following Mesa's public ac_build_sampler_descriptor encoding. Values outside
the reported +/-2 range and non-finite values are refused before conversion.
No layout or GPU address is persisted. Zero bias leaves old descriptor words
unchanged. Host checks cover both signs, fractions, range and non-finite inputs.

The new shader uses coordinate derivatives that select LOD 2 in a 256-square
five-level image. Eight frames change only the sampler bias: -2, -1, -0.5, 0,
0.5, 1, 2, then 0 again. Each mip has its own grey. Every RGBA pixel is checked,
including triangle edges; half-level biases must produce exact linear blends.

PS5 PID 250: 383 PASS, zero FAIL; all eight 8,294,400-pixel frames match exactly.
Original nearest and linear mip tests also pass. VideoOut's known busy warning
at unregister is followed by explicit title close and verified idle. All twenty
new driver submissions replay strictly, without comparison tolerances.

Reproduce from the driver root:

    python3 jobs/r26-lod-bias/check.py jobs/r26-lod-bias/readback.txt
    python3 jobs/r26-lod-bias/replay.py

Console: build the runner, deploy while idle, verify both served ELF reads and
all PT_LOAD segments, then run tools/ps5_console.py battery PPSA99988 with this
queue.txt. Listen before launch and explicitly close afterward. Captures are in
golden/r26-lod-bias; the raw log is ignored Klog_Logs/r26-lod-bias.log.

Explicit driver rebuild, full host driver/cache suite, all eleven gates, port
five gates/shader scan and template relink pass. Archive hash and deployment
proof are retained alongside this file. The game scaling retest is separate;
the probe alone makes no performance or gameplay claim.
