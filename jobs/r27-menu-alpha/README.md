# R27: restore pipeline blend control

vkQuake PID 253 isolates the black menu background to Draw_FadeScreen. Disabling
only the fade preserves the dimmed/desaturated world; disabling only the compute
menu effect still produces black. Port evidence: m6-menu-fade-diagnosis.

Pipeline creation calculated blend_control but never assigned it to the pipeline.
Commit cfab0b9 removed that assignment while adding rasterization state. Restore
that single assignment so every blending draw receives its requested control.

The new probe uses the game's 24-byte position/UV/RGBA8 vertex and sparse fragment
input locations 1/2. The bootstrap CLI package uses its supported BGRA format;
the actual Vulkan pipeline compiles RGBA, and both represent the same black RGB.
The raw-alpha frame proves normalized alpha 128 arrives intact. Three blended
frames over white check alpha 0, 128 and 255, every RGBA pixel across 3840x2160.
The existing constant-blend case covers separate colour/alpha factors.

PID 256 before: raw alpha correct, transparent and half-transparent frames each
have 8,294,400 wrong pixels. Constant blending also fails. The first attempted
probe (PID 255) encountered the harness's stale two-attribute guard; the backing
array already supports four. The guard now uses its declared maximum.

PID 257 first fix: 271 PASS, zero FAIL, all pixels correct. The capture tool keeps
one shader metadata pair per test, so its raw and blended exports could not both
replay in one case. Split those cases without changing the draw or comparisons.
PID 258 final: 285 PASS, zero FAIL, all four full frames and constant blend pass.
Five submissions replay exactly, without tolerances. The known VideoOut busy
warning is followed by close; console idle verified. Both deployed ELF reads and
all five PT_LOAD segments match. Failed baseline is retained in
 golden/r27-menu-alpha-before and before.txt; final captures in
 golden/r27-menu-alpha and readback.txt.

Reproduce from this repository:

    python3 jobs/r27-menu-alpha/check.py jobs/r27-menu-alpha/readback.txt
    python3 jobs/r27-menu-alpha/replay.py

For console: build runner, deploy while idle, verify deployed ELF, and use
 tools/ps5_console.py battery PPSA99988 jobs/r27-menu-alpha/queue.txt.
Listen before launch and close afterward. Raw logs remain ignored under
Klog_Logs/r27-menu-alpha-{before,final}.log.

Explicit driver rebuild, full 170-arm host/cache suite, eleven gates, port
five gates/shader scan and template relink pass. Archive and deployment proof
are recorded here. Game visual acceptance is recorded separately in the port.
