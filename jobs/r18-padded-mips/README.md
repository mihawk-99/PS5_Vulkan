# R18 padded mip chains

Port PID 210 identifies the refused image as 224x195 RGBA8, eight mip levels,
one layer, 2D, with a 256-texel stored pitch. The host runner reproduces this
exact refusal before the candidate change.

The candidate allows the existing custom-pitch descriptor for mip chains.
Acceptance requires every pixel in all eight pinned-LOD frames to match a
unique solid colour after Vulkan staging uploads. The queue also retains C4's
single-level padded texture and checks a 32x36 six-level chain. The separate
address case fills storage with byte offsets and logs nine sampled addresses
per mip; it is a measurement, not a pixel-layout acceptance test.

From PS5_Vulkan, explicitly build the driver before any linked checks:

```sh
bash tools/build-driver.sh
PS5VK_SHADER_CACHE_DIR="$PWD/build/host-regression-cache" bash tools/check-driver.sh c4_texture c7_mip_upload c7_tiled_mip
bash build/gates.sh
```

Inspect all eleven results, run the port gates and shader scan from its own
directory, and relink the template. With the console idle, deploy PPSA99988
and compare two executable reads and all PT_LOAD contents before launching:

```sh
python3 tools/ps5_console.py battery PPSA99988 jobs/r18-padded-mips/queue.txt --output Klog_Logs/r18-padded-mips.log --timeout 300
python3 tools/ps5_console.py kill PPSA99988
python3 tools/golden.py extract Klog_Logs/r18-padded-mips.log golden/r18-padded-mips
```

The runner requires the uploaded basename to be queue.txt.
Hardware results and the accepted or rejected scope follow below.

PID 211 rejects the guard-only candidate: 476 PASS, 33 FAIL. Single-level C4
passes; both padded chains fail. The address map measures reverse mip origins
matching AddrLib. Failed evidence: golden/r18-padded-mips-before. The corrected
queue is ../r18-padded-mips-fixed/queue.txt; it adds an aligned 256x256 five-level regression.

## 2026-09-23 — R18 row mip layout accepted, PID 214

The shared row-chain layout now places smaller levels before larger levels,
keeps each row aligned to 256 bytes, and sums all levels for layer size.
Non-array 2D mip descriptors supply the stored pitch even when the base width
is already aligned. Padded mip chains can now record. Arrays retain their
separate descriptor fields and guards; no port texture/visual workaround.

PS5 PID 214: 531 PASS, zero FAIL. All 19 pinned mip frames match all 8,294,400
pixels: 224x195/eight levels (the actual vkQuake image), 32x36/six levels,
and 256x256/five levels. Single-level C4 nearest/bilinear also passes.
All 21 command streams replay exactly. Two deployed ELF reads and all five
PT_LOAD segments match. Known benign VideoOut unregister-busy warning; title
closed and count=0 checked. Goldens: golden/r18-padded-mips-complete.
The verifier accepts PID 213/214 readbacks and rejects failed PID 211.

Explicit driver build: 14,434,994 bytes, SHA-256
cef1d81708d06d6fa68b2ac5df6b3f781c0fb59e3026e83e09ee469b112167fa.
Twenty-one targeted check-driver loader/direct/link arms, shader cache checks,
all eleven gates, port five gates/scan and template relink pass. The later
recorder-only change adds log_driver_stages; lint, unit and runner gates pass,
and the driver archive is unchanged. PID 213's successful pixel-only capture,
PID 211's failed candidate, and the misqueued PID 212 history are retained.
Reproduction: jobs/r18-padded-mips/README.md and the fixed queue beside it.
Next: vkQuake deployment and launch; M6 is not claimed.

Validate the readback records independently of command replay:

```sh
python3 jobs/r18-padded-mips/verify-readback.py golden/r18-padded-mips-complete/readback.txt
```
