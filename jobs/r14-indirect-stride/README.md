# R14 single-draw indirect stride

Question: does count=1/stride=0, used by vkQuake's indexed draws, record without
an assertion and preserve the existing command stream? Vulkan ignores stride
for a single draw. The shared draw helper now checks stride only for count > 1.
The host test covers both indirect commands, including refusal when earlier
recorded writes overlap their parameter buffer; it aborted before the fix.

Explicit build, six indexed/indirect test arms and eleven driver gates PASS.
Port five gates and shader scan PASS; template relink PASS. Archive 14425458
bytes, SHA-256 e32ba9c0d7f10063c4c9486248b9572b4e3c093b0b3c2c8d3ec1282f83963796.

PPSA99988 PID 204: m2-solid and c2-indirect, 121 PASS, zero FAIL. The latter
uses zero stride and checks its pixels on PS5. Indexed zero stride is covered
by the host shared-path test; this console case is non-indexed. One submission
replays exactly. Two deployed ELF reads and every PT_LOAD byte matched.
Known benign VideoOut unregister-busy warning; title closed afterwards.
No port launch in this cycle; dynamic-offset and tiled-chain refusals remain.

Reproduce:

```sh
bash tools/build-driver.sh
bash tools/check-driver.sh c2_indirect c2_indexed
bash build/gates.sh
python3 tools/ps5_console.py battery PPSA99988 jobs/r14-indirect-stride/queue.txt --output Klog_Logs/r14-indirect-stride.log --timeout 300
python3 tools/ps5_console.py kill PPSA99988
python3 tools/golden.py extract Klog_Logs/r14-indirect-stride.log golden/r14-indirect-stride
```

Replay uses the recipe in golden/r12-pitch/README.md with this golden directory
and case c2-indirect. readback.txt, deployed-proof.txt and host-replay.txt record
the measurements. [Vulkan stride rule](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndexedIndirect.html).
