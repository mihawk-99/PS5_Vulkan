# R91: the tiled colour maps, read off the GPU's own storage

R90's clears at 636x358 read back wrong in two format families, the same texels
in every run: 664 texels of R16G16B16A16 from (604,64), and 632 of R8_UNORM from
(8,356). The CPU's maps for one- and eight-byte elements are AddrLib's rows, and
no console run had checked them against where the hardware puts a texel.

The new case, `r91-tile-maps`, measures that directly. The r91-position probe's
fragment stage writes each fragment's own position into an unsigned integer
target (shaders/r91/position.frag): (x, y, 0xa5, 0x5a) for 16- and 8-byte
elements, (x & 255, y & 255, x >> 8, y >> 8) for four bytes and its first two
channels for two, and x & 255 and y & 255 in two frames for one byte. The
3840x2160 target's allocation is filled with 0xff first. The case then:

- checks every texel: the element the driver's CPU map names
  (`ps5vk_debug_image_texel_offset`, which is what the driver's copies, clears
  and readbacks use) has to hold that texel's own encoding;
- walks the storage tile by tile with no map, reading which texel each element
  holds, and counts the differences from the driver's offsets by value and by
  the tile's column and row parity. For one byte this covers the first two tile
  rows and the last one, from a static copy of the x frame. The case uses no
  large heap allocation: PID 236 stopped in operator new asking for a
  whole-target table.

PS5 PID 237, the maps as they were (readback-pid237.txt):

- 16-, 4-, 2- and 1-byte maps: every one of 8,294,400 texels where the GPU put
  it.
- 8-byte: 4,116,480 texels off, all of them by offset xor 0x800 (address bit
  11, in-tile x bit 5), in every odd tile row and nowhere else, in both
  tile-column parities. The hardware twists the tile row the way the
  sixteen-byte tile does, on x's bit 5 by the row's parity, and the CPU map did
  not. That twist, at R90's width, puts exactly 664 texels' partners past the
  image's right edge: 4 columns (604-607) by the 166 rows of odd tile rows.

The one-byte map was right texel by texel, so R8_UNORM's miss was in the copy
itself. The CPU copy moves sixteen-byte runs of a tiled row, and the one-byte
map keeps only eight bytes contiguous: its address bits 0-2 are x's bits 0-2,
and bit 3 is y's bit 1. The second half of every run came from the row two
below. Under a uniform clear that row holds the same value, except where it
lies past the image: rows 356 and 357 of 358, at x with bit 3 set, which is
R90's 632 texels. Uploads and readbacks of one-byte tiled images had moved half
their texels from the wrong row this way everywhere.

The fixes (driver/ps5vk_image.c, driver/ps5vk_queue.c):

- the eight-byte map applies `in_x ^= 32` on odd tile rows;
- a tiled one-byte side runs eight bytes, the others sixteen as before.

PS5 PID 238, 4 of 4 (queue.txt; readback.txt): `r91-tile-maps` six frames of
six, every texel of every element size where the GPU put it; `r90-image-clears`
30 of 30 clears exact, R16G16B16A16_UNORM, _SFLOAT and R8_UNORM back in the
case; v0-targets and v0-targets-uint pass. No GPU fault.

In the last, partial tile row the storage walk also reads elements no texel
wrote. They decode as texels of other tiles (61,440 for four bytes) or repeat a
texel (two and one byte). The per-texel check never reads them.

Reproduce from the driver root:

    python3 jobs/r91-tile-maps/check.py

Console: build the probe (tools/build-probe-shaders.sh r91-position), the
driver and the runner, deploy while idle, then run tools/ps5_console.py battery
PPSA99988 with queue.txt. The raw logs are the ignored
Klog_Logs/r91-tile-maps-1.log (PID 236), -2.log (237) and -3.log (238).
