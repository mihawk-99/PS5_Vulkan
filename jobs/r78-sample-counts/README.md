# R78: two and eight samples, and every pixel's sample locations

The driver rendered one or four samples. It now renders, samples and resolves
two and eight as well: the tile of a 64 KiB swizzle at those counts (AddrLib's
gfx10 block rule, 128x64 and 64x32 four-byte texels), the counts' log2 in
CB_COLORi_ATTRIB, DB_Z_INFO, PA_SC_AA_CONFIG and DB_EQAA, and RADV's sample
locations, distances and centroid priorities; eight samples take each pixel's
second locations register for samples 4-7.

The same round corrected where the locations go. Each pixel of the 2x2 quad has
four locations registers, so the quad's pixels are 0x2fe, 0x302, 0x306 and
0x30a; C8 wrote 0x2fe, 0x300, 0x302 and 0x304, which left the lower two pixels
(X0Y1, X1Y1) at AGC's defaults.

`r78-sample-locations` draws a band whose two edges run through pixel centres,
one on an even line and one on an odd one, at 2, 4 and 8 samples, vertically
and horizontally, and resolves it on the GPU. With the standard locations half
of every edge pixel's samples are covered, so every texel of both edge lines is
the half-way blend of the band and the clear colour.

Console: PID 790, all six frames PASS (every edge texel 0xff80409f, no wrong
texel), c8-msaa and c8-resolve PASS and captured into their goldens. PID 791,
the same probe with C8's register offsets restored: FAIL -- 2160 wrong texels
on each vertical edge pair and only one of the two horizontal edges blended at
every count, the odd line's pixels covering all or none of their samples. That
is what four-sample rendering did before this round.
