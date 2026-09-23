# R17 sampled-image descriptor arrays

vkQuake's lightmap compute shader declares three SAMPLED_IMAGE descriptors at
set 0 binding 2. PID 208 refused this before dispatch. The driver now stores
one record per element, maps each binding to its first record, and handles
multi-element writes/copies and partial updates. The shared graphics/compute
table writer validates and emits each element at offset + element * stride.
Dynamic UBO offsets follow the same element order; input-attachment arrays
still require their own subpass-index witness.

The new compute probe has a separate sampler at binding 0, an unused binding
number 1, three sampled images at binding 2, and a storage output in set 1.
It writes a source set, copies all three records to the draw set, copies two
records at destination element 1, overwrites element 0, and changes the source
after copying. The final draw order must be [image 2, image 0, image 1]. The
host compares all three emitted addresses against their real image storage.
The shader takes red, green and blue from those three distinct images; PS5
readback must match all 256 output texels. This covers copies by value as well
as nonzero array indices and the hole in binding numbers.

Reproduction from PS5_Vulkan:

```sh
bash tools/build-driver.sh
bash tools/build-compute-probe.sh r17-descriptor-array
PS5VK_SHADER_CACHE_DIR="$PWD/build/host-regression-cache" bash tools/check-driver.sh
bash build/gates.sh
```

Inspect all eleven gate results, then run the port gates/scan from the port
directory and relink the template. Deploy the driver runner only with the
console idle; verify two executable reads and every PT_LOAD segment. Then:

```sh
python3 tools/ps5_console.py battery PPSA99988 jobs/r17-descriptor-array/queue.txt --output Klog_Logs/r17-descriptor-array.log --timeout 300
python3 tools/ps5_console.py kill PPSA99988
python3 tools/golden.py extract Klog_Logs/r17-descriptor-array.log golden/r17-descriptor-array
```

The queue retains the scalar compute-image case as a regression. Host pixel
checks cannot establish GPU correctness; the host checks descriptor addresses
and compares captured command streams. Hardware acceptance will be appended
after readback, PID correlation and idle closure.


## 2026-09-23 — R17 descriptor arrays accepted, PID 209

The user requested R17 then R18. Each descriptor array element now has its own
record; writes, copies and partial updates use binding record indices. Shared
graphics/compute validation and emission walk elements at their declared stride.
Dynamic offsets retain binding/element order. Input-attachment arrays remain
refused pending a subpass-index witness.

PS5 PID 209: 104 PASS, zero FAIL. Both scalar d2-compute-images and the new
r17-descriptor-array produce all 256 exact output texels; two streams replay
exactly. The new case writes/copies/partially updates a three-image array,
changes its source set afterwards, and requires final order [2,0,1]. Host direct
checks confirm all three emitted image addresses. Title closed; count=0 checked.
Two deployed ELF reads and all five PT_LOAD segments match.

Explicit driver archive: 14,434,234 bytes, SHA-256
aad0ebc750f06f00e130b524e4ebe055a8d190682a9abfbdba2ab105935f634a.
Full check-driver initially 169/170 PASS: capability direct expected compiler
stderr but a cache hit skipped compilation. That warning-specific test now
explicitly disables cache; its three loader/direct/link arms PASS. Eleven gates,
port five gates/scan and template relink PASS. No runtime cache change.
Evidence/reproduction: jobs/r17-descriptor-array and golden/r17-descriptor-array.
R18 exact padded image shape remains next; no vkQuake retry yet.
