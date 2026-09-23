# R15 independent dynamic uniform offsets

Question: can vkQuake bind two dynamic uniform buffers in one set, with each
reading its own offset? The layout now numbers dynamic bindings in ascending
binding order. Command buffers keep each offset separately; static buffers get
none. Graphics and compute share this descriptor writer. Meta save/restore and
command reset retain/clear the offsets alongside their sets. The storage holds
the advertised eight dynamic UBOs per set; descriptor arrays remain refused.

The new r15-dynamic-pair probe declares bindings in reverse order: dynamic 5,
static 2, dynamic 0. Two frames swap offsets 0 and 16. Red reads binding 0,
green reads 5, blue reads static 2. Descriptor address/range checks run on the
host and console; PS5 checks the frame pixels independently. Correct words are
0xff80d0ff and 0xff804030. The ordinary D1 case is in the same queue.

Acceptance: PPSA99988 PID 205, 196 PASS, zero FAIL; both cases and all four
frames pass. Four submissions replay exactly. Golden evidence, readback,
deployed PT_LOAD verification and replay output: golden/r15-dynamic-pair.
Known benign VideoOut unregister-busy warning; title closed. No vkQuake launch
in this cycle. Water mip blits remain before the next port experiment.

Explicit build: 14427682 bytes, SHA-256 055c7c6c1ff47829fcb3c294cc4d8bd758529a0c2dea8f238e3347ed11c17811.
Full check-driver: 165/167 passed initially; the new binding walk dereferenced
an empty set in the subpass harness. Host debugger identified the line; keeping
an empty slot empty fixes it. The final rebuild and all nine D1/subpass/present
arms pass. Eleven driver gates, port five gates and template relink pass.

Correction to earlier D1 coverage: update_uniform_descriptor wrote an ordinary
UNIFORM_BUFFER into a layout declared UNIFORM_BUFFER_DYNAMIC. Prior pixels
proved the address offset, but not correctly typed Vulkan descriptor writes.
The harness now writes the declared dynamic type, and shader resource checking
accepts it. Original D1 streams still compare unchanged. Earlier logs/goldens
are preserved; this entry narrows the earlier claim explicitly.

Reproduce:

```sh
bash tools/build-driver.sh
bash tools/check-driver.sh
bash build/gates.sh
python3 tools/ps5_console.py battery PPSA99988 jobs/r15-dynamic-pair/queue.txt --output Klog_Logs/r15-dynamic-pair.log --timeout 300
python3 tools/ps5_console.py kill PPSA99988
python3 tools/golden.py extract Klog_Logs/r15-dynamic-pair.log golden/r15-dynamic-pair
```

Host runner/replay recipe: golden/r12-pitch/README.md, with this golden directory
and cases d1-dynamic-ubo and r15-dynamic-pair. The pair emits two
r15_dynamic_tables PASS records even on the host; GPU pixel checks require PS5.
[Offset ordering](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html).
