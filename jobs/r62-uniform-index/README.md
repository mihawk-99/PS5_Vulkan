# R62: uniform buffers are byte ranges, not one 16-byte structure

Wind Waker's 3D scenes were one flat fog colour and untextured. A debug mode in
Dolphin's fog shader showed every fragment at the far end of the depth range,
with plausible fog constants on the CPU side: the vertex shader's depth came
out wrong. Dolphin selects each vertex's matrices by indexing uniform arrays
(ctrmtx[posidx]), which no console probe had covered.

The probe's vertex shader reads rows[index.x] from a 64-row uniform array, the
index an R8G8B8A8_UINT attribute. Before the fix every pixel read 0x00000000;
row 0 read correctly and row 1 read zero. The uniform descriptor was the M3
canary's structured form -- STRIDE 16, NUM_RECORDS the count of 16-byte rows,
OOB_SELECT 0 -- under which the hardware checks each load's byte offset against
the stride, so a vector load (a run-time index) past the first 16 bytes was out
of range. Scalar loads at constant offsets, all that earlier applications used,
are checked against the whole range and were unaffected.

Uniform buffers and the push-constant block are now written as RADV writes them
for GFX10 and later: STRIDE 0, NUM_RECORDS the range in bytes (rounded up to
whole 16-byte rows as before), OOB_SELECT raw (2).

PS5 PID 362: rows 3, 17, 40 and 63, row 0 in every band and row 1 in every
band all PASS with zero mismatches over 8294400 pixels; r15-dynamic-pair (its
table check now expects the byte range), d1-dynamic-ubo, v0-push-constant and
R57-R61 pass in the same run. The submission replays on the host word for word.

Host gate: driver/tests/ps5vk_test.h's uniform-entry check (used by the two
multiset tests) asserts the raw form: no stride, the range in bytes, OOB_SELECT
raw.

Reproduce from the driver root:

    python3 jobs/r62-uniform-index/check.py jobs/r62-uniform-index/readback.txt
    python3 jobs/r62-uniform-index/replay.py
