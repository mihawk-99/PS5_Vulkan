# R13 region upload records — console PID 198

Question: preserve uploaded/copy pixels while recording one row-image upload
region once, rather than one 272-byte record per row. Host metadata measurements
and runnable GDB stress witness: `../../jobs/r13-upload/README.md`.

PPSA99988 PID 198, `jobs/r13-upload/queue.txt`: m2-solid, c4-padded,
c7-mip-upload and c7-copy PASS. 486 PASS records, zero FAIL; the known benign
VideoOut unregister-busy warning preceded closure. The runner was closed.
Readback probe results: `readback.txt` (JSON). Twelve submissions replay exactly
with this run's m2-solid register defaults (`host-replay.txt`); older goldens
are unchanged. This proves driver bytes/streams, not yet the port's full map.

Archive: 14,383,804 bytes, SHA-256
`b3bb7ac99730e894895fb817f956eb48616eaa856dc1fe1ffd3d31b8221e52f0`.
Two deployed ELF reads match each other and every PT_LOAD byte of the local
build/eboot.elf; hashes are in `deployed-proof.txt`. The console converts SELF,
so this compares program bytes rather than the container's trailing metadata.

Hardware reproduction after the explicit driver/runner builds and deployment:

```sh
python3 tools/ps5_console.py battery PPSA99988 jobs/r13-upload/queue.txt --output Klog_Logs/r13-upload.log --timeout 300
python3 tools/ps5_console.py kill PPSA99988
python3 tools/golden.py extract Klog_Logs/r13-upload.log golden/r13-upload
```

Host replay: use the recipe in `../r12-pitch/README.md`, changing the golden/work
directories to r13-upload/r13-replay and running c4-padded, c7-mip-upload and
c7-copy. The host verifies submission words; only the PS5 proves rendered pixels.
