# R42: resample blits on several threads

Walking the start map (port fixture `build/r41-walk-autoexec.cfg`, evidence
`m6-r41-walk`), the CPU water-warp mip blits cost 2.3-8.2 ms a frame depending
on what is in view, and with the application's flat ~16.7 ms that pushed the
frame over the display's ~20.8 ms VRR window: those frames present at 29.25 ms
(34 FPS), and walking swings the game between ~50 and 34 FPS.

## The change

`ps5vk_blit_execute`'s texel loop is now `ps5vk_blit_rows` over a range of rows;
the single-thread path calls it for all rows and is otherwise unchanged.
Consecutive blit records at one split point form a wave while no two touch the
same image (each record carries its whole source and destination image span;
a record without spans runs alone), so a mip level that reads the level before
it stays in order. A wave's records are cut into row ranges taken from a shared
counter by four pool workers and the submitting thread. Rounds are published
under the pool lock only while no worker is inside one, because a worker that
woke late for the previous round could otherwise read parts a realloc frees.
Source spans are invalidated before, touched ranges flushed after, as before.
No worker: the old serial path.

## Verification

Host: build clean, 11 gates, `check-driver.sh` PASS, `check-shader-cache.sh`
PASS, `jobs/r29-tile-address/replay.py` and `replay-integer.py` identical.
Console runner (archive `archive.txt`), every test that blits, builds a mip
chain or resolves (`blits/queue.txt`): 19 of 19 PASS, statuses identical to the
pre-change driver (`baseline.txt` from `jobs/r37-mapped-flush`), r16-mip-blit
0 mismatched texels in each of its four generated levels (`runs/`).

Game (port `m6-r42-parallel-blit`, same walk, profiled): CPU copies 2.3-8.2 ->
0.4-1.2 ms a frame; walking FPS 34-47 -> 52.4-55.4; frames at the 29 ms VRR
cliff 109-342 -> 1-12 a window. Left: one 151 ms hitch mid-walk and the
settling after the map load.
