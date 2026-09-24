# R61: a one-layer texture sampled through a 2D-array view

Dolphin samples every texture with sampler2DArray through a one-layer
2D_ARRAY view, and the driver describes such a view as a plain 2D image (its
array flag follows the layer count). I suspected that for Wind Waker's missing
textures and measured it before changing anything: it is correct.

The probe draws the v0-array set's two bands (layer coordinates 0 and 1) over a
one-layer image through a 2D_ARRAY view; both coordinates clamp to layer 0, so
every pixel is that layer's colour. Frame 0 is a linear image (Dolphin's
textures), frame 1 a tiled one (its EFB copies).

PS5 PID 363: both frames PASS, zero mismatches over 8294400 pixels each
(0xffc08040). It also passed on the earlier driver (PID 342), before any change.

The host replay is not word for word: the host's free-memory model places the
draw's descriptor-table chunk 0x40000 lower than the console did (the tables'
user-data pointers differ; every other word is identical), so this job keeps the
console readback as its gate and has no replay script.

Reproduce from the driver root:

    python3 jobs/r61-one-layer-array/check.py jobs/r61-one-layer-array/readback.txt
