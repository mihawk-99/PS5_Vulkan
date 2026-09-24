# R63: Dolphin's skinned vertex record

Wind Waker's animated characters drew with stretched polygons while the scenery
drew in place. Dolphin gives a skinned vertex a 36-byte record whose first four
bytes are a matrix index (R8G8B8A8_UINT), with the position at offset 4, the
normal at 16 and a texture coordinate at 28, where the scenery's records start
with the position. I suspected the fetch of that record, a four-byte attribute
ahead of three float ones, and measured it before changing anything: it is
correct.

The probe's vertex shader reads all four attributes from that record: a band's
place comes from its position, its colour from its normal's x, its
coordinate's y and the uniform row (R62) its index selects, so any field read
from the wrong bytes moves or recolours the band.

PS5 PID 376, and again in R64's run (PID 449, whose readback and golden these
are): the four bands PASS with zero mismatches
over 8294400 pixels. Every Dolphin draw goes through primitive restart, and
R64 (jobs/r64-restart-strips) found long restart draws broken, which is where
the stretched polygons point instead. This job stays as a gate on the record's
layout. The submission replays on the host word for word.

Reproduce from the driver root:

    python3 jobs/r63-skinned/check.py jobs/r63-skinned/readback.txt
    python3 jobs/r63-skinned/replay.py

Console: deploy the runner while idle, run tools/ps5_console.py battery
PPSA99988 with this queue.txt, then restore jobs/regression/queue.txt.
