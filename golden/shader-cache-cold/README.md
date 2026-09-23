# Shader cache cold probe — PID 200

PPSA99988, jobs/shader-cache/queue.txt: m2-solid, c4-padded, c4-texture,
d2-compute all PASS. 241 PASS records, zero FAIL. Known benign VideoOut
unregister-busy warning before closure. Both direct and indirect compute
read back 0xa5a5a5a5; texture pixels passed both sampler checks.

The unchanged binary was used for PID 200 then PID 201. All six captured
submissions replay exactly with same-run m2-solid defaults (host-replay.txt).
Two deployed ELF reads matched every PT_LOAD byte (deployed-proof.txt).
The probe's stdout does not expose cache-hit counts to its structured kernel
log; the vkQuake cold/warm trace is the separate cache-hit/timing witness.
No older golden was regenerated.

Archive: 14,425,130 bytes, SHA-256
e089e0608def7d4f2b100e0b5ee1713b1d2b6c2ed8cd7a59f261f8686efeb44f.
Reproduce: python3 tools/ps5_console.py battery PPSA99988 jobs/shader-cache/queue.txt
--output Klog_Logs/cache-probe-cold.log --timeout 300, then kill PPSA99988.
Extract with tools/golden.py extract and replay the three driver cases with
the ../r12-pitch/README.md recipe using this directory.
