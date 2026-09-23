# R16 water-texture mip blit — accepted on PS5

Question: can the driver generate and sample all four lower levels of vkQuake's
512x512, five-level water texture using linear same-image blits? The probe
uploads a 0/254 checker, blits each level in turn, then pins each lower mip and
checks every output pixel for RGBA 0xff7f7f7f. CPU checks cover all 87,040 lower
texels. No visual settings change.

## Failed hardware run and correction

PPSA99988 PID 206 returned 271 PASS, five FAIL and one known benign VideoOut
unregister warning. Levels 1, 2 and 4 passed all 8,294,400 pixels. Level 3
(64x64) matched only 7,776,000 pixels; the remaining 1/16 read zero. CPU checks
using the old additive addressing passed, so they were not independent proof.
The title was closed, the console was checked idle, and no vkQuake run followed.
Evidence: ../../golden/r16-mip-blit-before, including deployed PT_LOAD checks,
readback and ten exact command-stream replays. Stream equality does not prove
the corrected CPU writes reached the GPU.

Whole-chain AddrLib 64KB_R_X queries reveal 2,048 wrong texel addresses in each
of the 256x256/five-level and 512x512/five-level chains. XOR of the tail origin
with the local swizzle gives zero mismatches across 87,296 and 349,184 texels.
Earlier C7 origin/centre checks did not establish whole-chain correctness.
The correction is appended separately in HARDWARE_FINDINGS.md and M5_PHASE_C.md;
old records and failed evidence remain intact.

## Host verification before resumption

The checkpoint patch at edb8ebd:parked/r16-mip-tail.patch applies to b838832. It adds the measured
512x512/five-level chain, permits mapped colour-chain transfers, uses XOR for
tail addressing in shared upload/copy/blit paths, and flushes the range actually
written. The independent CPU witness uses shifted tail coordinates. Probe and
whole-chain oracle checks are included in the patch.

Explicit rebuild had zero warnings; fifteen targeted check-driver arms and all
eleven gates passed. Port five gates and template relink passed. Upload metadata
remains one record per region: 64 records use 18,432 bytes, below the 32 KiB
heap. See validation.txt, oracle.txt, host-check.txt and candidate-archive.txt.
host-check-before.txt deliberately retains the misleading old CPU witness.
At that checkpoint the corrected candidate had NOT run on PS5.

The mission says to stop when a run contradicts an earlier claim. PID 206
triggers that rule. The patch was parked and accepted R15 source/archive and port links restored.
The user then explicitly resumed corrected readback and the vkQuake launch.

## Reproduction

From PS5_Vulkan:

```sh
bash tools/build-driver.sh
PS5VK_SHADER_CACHE_DIR="$PWD/build/host-regression-cache" bash tools/check-driver.sh c7_mip_upload c7_tiled_mip c7_copy c7_blit_formats c4_texture
bash build/gates.sh
```

Inspect all eleven gate results: the local aggregator does not propagate a
failed child exit. Run port gates and its shader scan from the port directory,
and rebuild ps5-homebrew-template against this archive. Follow the existing
deployment procedure: idle console, runner param/build definitions, two reads
of the deployed ELF and PT_LOAD identity check, listener before launch. Then:

```sh
python3 tools/ps5_console.py battery PPSA99988 jobs/r16-mip-blit/queue.txt --output Klog_Logs/r16-mip-blit-corrected.log --timeout 300
python3 tools/ps5_console.py kill PPSA99988
python3 tools/golden.py extract Klog_Logs/r16-mip-blit-corrected.log golden/r16-mip-blit-corrected
```

Require all four pinned mip frames and all independent CPU checks to pass,
no refusal, correct PID, exact replay, and idle closure. Do not overwrite the
failed goldens. Only then accept the driver change, relink/deploy vkQuake by
content, and run it to find the next measured M6 issue.

## 2026-09-22 — Corrected hardware acceptance

PS5 PID 207: 277 PASS, zero FAIL, all three queued cases PASS. All four pinned
lower mip frames matched 8,294,400/8,294,400 pixels. The independent CPU check
matched all 87,040 lower texels in each frame. Ten captured submissions replay
exactly (six C7 upload, four R16 blit). Known benign VideoOut unregister-busy
warning; title closed and count=0 confirmed. Evidence is
../../golden/r16-mip-blit-corrected; the earlier failed evidence is retained.

Explicit rebuild reproduces candidate-archive.txt byte-for-byte. Fifteen
check-driver arms, cache checks, all eleven gates, port five gates, shader scan
and template relink passed before launch. Two deployed ELF reads and all five
PT_LOAD segments matched. The actual source change is now accepted; the parked
patch remains available in checkpoint edb8ebd. Next: relink and launch vkQuake.
