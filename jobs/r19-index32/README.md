# R19: 32-bit indexed draws

vkQuake PID 215 reaches map recording, then the shared draw path refuses
UINT32 indices. The host reproduces that exact refusal using the C2 indexed
shader replay. The candidate uses the element width consistently for bounds,
firstIndex and the native index-size call; the host AGC model gains UINT32.

The probe has 65,540 vertices, with a white fullscreen quad at indices
65,536..65,539. Three sentinel elements precede the six valid indices.
firstIndex=3 and indexCount=9 must clamp to six elements, at byte offset 12.
Direct, indirect and secondary paths each check the actual emitted packets
and every output pixel. The queue runs C2's 16-bit indexed regression before and after UINT32.

```sh
bash tools/build-driver.sh
PS5VK_SHADER_CACHE_DIR="$PWD/build/host-regression-cache" bash tools/check-driver.sh c2_indexed c2_indirect b8_secondary v0_robust c2_staging c2_instancing
bash build/gates.sh
```

Inspect all eleven gate results; verify the port and its shader scan, and
relink the template. Before console deployment, use the existing C2 replay
with the driver host runner and queue r19-index32. Check its log with:

```sh
python3 jobs/r19-index32/check.py build/r19-state-host/stdout.log
```

The host does not render; only packet, offset and bounds checks can pass there.
On the idle console, deploy with the existing runner parameters, verify two
executable reads and every PT_LOAD segment, then capture and close:

```sh
python3 tools/ps5_console.py battery PPSA99988 jobs/r19-index32/queue.txt --output Klog_Logs/r19-index32.log --timeout 300
python3 tools/ps5_console.py kill PPSA99988
python3 jobs/r19-index32/check.py Klog_Logs/r19-index32.log --pixels
python3 tools/golden.py extract Klog_Logs/r19-index32.log golden/r19-index32
```

Pixel acceptance and command-stream replay are required before game relaunch.


The first candidate's host check found that index size was cached across
recordings: indirect/secondary frames omitted the size packet. The final
candidate writes size at every indexed draw and removes the cache field.
Historical goldens are not replaced. check-driver uses the explicit
--index-size-rebind migration for its old UINT16 captures: every new indexed
draw must have its own exact UINT16 write; missing/wrong writes fail, and all
other packets/tables still compare. A unit check covers those rejection cases.
New R19 goldens use strict comparison without this option.

## Accepted console run

PID 216: 257 PASS, zero FAIL. All three UINT32 frames match 8,294,400 pixels;
UINT16 before/after passes. Five streams replay exactly without migration.
See ../../golden/r19-index32/{readback,replay,deployed-proof}.txt.
Host: 170 driver arms, eleven gates, port five gates/scan and template pass.
Archive SHA-256: f2666ab80aadfb5a64722f9b7f014dd29c9c595462e9a1ae65d854ddbd26d411.
