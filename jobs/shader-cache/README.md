# Persistent shader cache

Question: can an unchanged title reuse shader compiler outputs across process
lifetimes, preserve exact GPU packages, and recover from stale or broken files?

`tools/check-shader-cache.sh` checks content keys, specialization values versus
pointer addresses, compiler options, exact output restoration, corruption and
truncation. It launches the real C4 texture test in two fresh processes and
compares vertex/pixel packages byte for byte, requiring warm cache hits with
no SPIR-V compile. `tools/check-driver.sh` runs it whenever C4 texture is selected.

On PS5 the default directory is `/app0/ps5vk-shader-cache`; each successful
SPIR-V compile is flushed and atomically renamed there before returning. A game
crash therefore preserves completed shaders. Normal deployment uploads files
without removing this directory. `PS5VK_SHADER_CACHE_DIR` overrides the path;
an empty value disables it. Host caching is opt-in through that variable.

The key hashes the SPIR-V, entry point, every compile option, specialization
map and data, and a generated digest of compiler archive, relevant driver
sources, headers and build flags. Addresses and struct padding are not keys.
Read failures, truncated/corrupt entries or unwritable storage fall back to
compilation. Only immutable compiler output is saved: no GPU pointers, shader
objects, or pipeline resources. Internal NIR meta shaders still compile; they
are not the hundreds of application stages responsible for the startup wait.

The optional Vulkan application cache API remains empty; this disk cache also
works when an application creates pipelines with VK_NULL_HANDLE. Old namespaces
remain on disk; no automatic eviction is needed for this title-sized cache.
Remove the title's cache directory to reclaim them or request a fresh compile.

## Hardware acceptance

Same vkQuake binary, identity 78bd43a2e575089a96cf8dc561937dd7781c462fbcf051f2fa177ac0c55107b1:

| Run | PID | Launch to first present | SPIR-V compiles | Cache hits | New entries |
| --- | --- | --- | --- | --- | --- |
| Cold | 202 | 30.410 s | 99 | 433 | 99 |
| Warm | 203 | 13.018 s | 0 | 532 | 0 |

Eight internal NIR stages compile in both runs. Times include launch IPC and
one-second trace polling; they measure first QueuePresent success, not an
optical display timestamp. Cold/warm JSON summaries are in cold-startup.txt
and warm-startup.txt. Each uses two identical final FTP reads, newest boot
identity only, listener-before-launch PID correlation, and confirmed idle
closure. The cold title crashed during map recording after presentation; the
warm run nevertheless reused all its saved SPIR-V stages. The same known
map blit/dynamic-offset refusals and indirect-stride assertion remain. These
paired runs measure the requested cache, not a repair of that rendering fault.

Run the paired measurement from PS5_vkQuake after gates and verified deployment:

```sh
python3 tools/check-shader-capabilities.py
python3 ../PS5_Vulkan/jobs/shader-cache/benchmark-vkquake.py cold
python3 tools/check-shader-capabilities.py
python3 ../PS5_Vulkan/jobs/shader-cache/benchmark-vkquake.py warm
```

Do not clear an existing cache to repeat a cold claim without recording that
action: the script labels runs but never deletes cache data. Startup evidence
is also committed in the port's evidence/m2-shader-cache-cold and -warm. Driver
probe PIDs 200/201 each passed 241 checks; all twelve submissions replay exactly
(golden/shader-cache-cold and golden/shader-cache-warm). Explicit driver build
had zero warnings; 167 test arms, cache checks, eleven driver gates, port five
gates, and template relink passed.
