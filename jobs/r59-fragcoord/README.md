# R59: gl_FragCoord's z and w in a fragment shader

Wind Waker's 3D scene was one flat fog colour (../PS5_RetroArch, 2026-09-24):
with Dolphin's fog disabled the geometry appeared. Dolphin's fog reads the
fragment's depth from gl_FragCoord, and no console probe had ever read
gl_FragCoord.z or .w, so this measures them before any fix is attempted.

A full-screen quad's vertex shader sets depth from 0.25 on the left edge to 0.75
on the right, with clip w 2; the fragment shader writes gl_FragCoord.z to red and
gl_FragCoord.w to green.

PS5 PID 337 (and a first run, PID 336): PASS, zero mismatches over all 8294400
pixels. Red follows the depth ramp to within one step in every column (0x40 at
the left edge, 0x80 in the middle, 0xbf at the right) and green is 0x80 (0.5)
everywhere. The fragment position's z and w are correct, so the fog defect is
elsewhere; this probe stays as the regression gate for the path.

Reproduce from the driver root:

    python3 jobs/r59-fragcoord/check.py jobs/r59-fragcoord/readback.txt
    python3 jobs/r59-fragcoord/replay.py
