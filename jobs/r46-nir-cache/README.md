# R46: land the internal NIR shader cache

`parked/nir-shader-cache/driver.patch`, applied unchanged: the eight internal
NIR stages (meta shaders) that compiled on every launch now use Mesa's
pointer-free NIR serialization as the key input to the existing persistent
output cache, with a zero prefix to tell NIR from SPIR-V.

Host, on this tree: build clean; `parked/nir-shader-cache/check.py` PASS
(fresh-process NIR cache skips compilation, byte-identical output, stable
keys); `replay.py` PASS (cold 4 compiles incl. 2 NIR, warm 0 compiles and 4
hits, eight exact mip submissions); `check-driver.sh`, 11 gates and
`check-shader-cache.sh` PASS.

Console:
- Game, same binary cold then warm (port evidence m6-r46-nir-cache): cold 102
  compiles, pipelines 2.05 s; warm **0 compiles** (every warm launch before this
  compiled 8 NIR stages), pipelines 0.10 s.
- Runner (archive `archive.txt`), mip and menu-alpha cases twice on one binary:
  9/9 PASS both times, identical to the pre-change driver, r16-mip-blit 0
  mismatched texels. The runner's compile lines do not reach its kernel log,
  so its cold/warm counts are not observable; the game's are the proof.
