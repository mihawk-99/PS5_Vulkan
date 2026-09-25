# R66: uniform buffers bound their loads (OOB_SELECT raw)

R62 made uniform and push-constant descriptors byte ranges and wrote word 3's
OOB_SELECT as 2, which its comment called raw. Mesa's register header names 2
DISABLED: the only out-of-range case is NUM_RECORDS == 0, so a load past the
bound range read whatever memory followed it, past the end of the buffer
included. The driver reports robustBufferAccess, which forbids that. RAW is 3
(offset >= NUM_RECORDS is out of range and reads zero), which is what RADV
writes for GFX10 and later (ac_build_raw_buffer_descriptor). I found it reading
the descriptor code while looking at storage buffers, which already used 3.

The probe draws R62's four bands (rows 3, 17, 40, 63 of a 64-row uniform array,
the row picked by an R8G8B8A8_UINT attribute) with 16 rows bound: row 3 is in
range, and rows 17, 40 and 63 must read zero, a transparent black pixel. The
control frame binds all 64 rows.

Before the fix (PID 511, before-readback.txt): rows 17, 40 and 63 drew their
real colours (0xffdfba45, 0xff615da2, 0xffe700ff), 2073600 wrong pixels in each
band. With OOB_SELECT 3, PID 512: both frames PASS with zero mismatches, the
three bands past the range read 0x0; R62, r15-dynamic-pair, d1-dynamic-ubo,
v0-push-constant and R63 pass in the same run. The submission replays on the
host word for word (descriptor contents are not part of the replayed words).

Host gate: driver/tests/ps5vk_test.h's uniform-entry check (the multiset tests)
asserts OOB_SELECT 3.

Reproduce from the driver root:

    python3 jobs/r66-uniform-bounds/check.py jobs/r66-uniform-bounds/readback.txt
    python3 jobs/r66-uniform-bounds/check.py jobs/r66-uniform-bounds/before-readback.txt --before
    python3 jobs/r66-uniform-bounds/replay.py

Console: deploy the runner while idle, run tools/ps5_console.py battery
PPSA99988 with this queue.txt, then restore jobs/regression/queue.txt.
