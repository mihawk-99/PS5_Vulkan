# Shared PS5 graphics broker

This optional backend lets trusted native clients use a title's GPU and display.
It has no Wine headers, process launcher, title ID or game-specific behavior.
PS5_Mesa owns the RADV overlay; PS5_Vulkan owns these runtime files and recipes.

## Build and use

`tools/build-gpu-broker-client.sh OUTPUT SDK [WSI=1] [DRAW=0]` builds a complete
RADV client archive against the already-built release archive. The base archive
SHA-256, Mesa base and overlay commits, SDK revision and source hashes are
recorded in `foundation-manifest.json`. It fetches nothing. Ordinary title
RADV builds keep their existing platform and VideoOut backend.

An owner compiles `server.cpp`, `display.cpp` and `packet_io.c`, including the
pinned RADV platform headers, and links with `tools/radv-link.sh`. Start with
`ps5_gpu_broker_start(directory, capture, local, is_absent)`, then transfer the
endpoint returned by `ps5_gpu_broker_open` into a trusted client process. The
application owns process creation, desktop focus and input. Give its opaque
lifecycle token to close; `is_absent(token)` must prove the native process ended.
Socket EOF alone never permits backing reclamation. Stop only after all clients
close; pending GPU fences drain before buffers are freed. `ps5_gpu_broker_flips` and
`ps5_gpu_broker_capture_next` give the application the frames shown so far and a picture
of the next one, for scripted runs of programs that present through the broker. Failed drains retain
backing and report device loss. The owner title must use its shell-close or
LoadExec path, never `exit()` or return from `_start`.

Before the client runtime maps memory, reserve `protocol.h` host ranges. Bind
its endpoint with `ps5_gpu_client_bind(fd, reserved)`, then use Vulkan through
the ICD entry points in `client.a`. Clients own independent RADV state; the
owner submits their commands and presents completed linear images. The endpoint
is borrowed and must stay live for the client's lifetime. Shared addresses,
64-byte packets, allocation bounds and descriptor cleanup preserve the former
Proton contract. The trusted-client boundary is deliberate: this is not an
untrusted GPU command sandbox. The frame-page layout and gpu-wine log labels
retain compatibility with existing consumers/evidence parsers.

`tools/check-gpu-broker.sh` is the standalone ASan/UBSan host check. It checks
bounds, packet and descriptor transfer, endpoint rendezvous and truncation cleanup.
Proton's native synthetic probe includes the owner implementation to exercise
its internal allocator/fences; ordinary consumers compile it directly.

## Optional Windows OpenGL frontend

`tools/build-mesa-windows-zink.sh WORK MESA_FORK LLVM_MINGW_ARCHIVE` builds PE64
`opengl32.dll` and `libgallium_wgl.dll` from a pinned PS5_Mesa commit. It verifies
the compiler archive and exports the Mesa revision through git archive, without
network access. Its build manifest records the sources and DLL hashes. WGL is
an optional Windows frontend: native PS5 EGL/OpenGL support is not implemented.
Wine DLL staging, overrides and prefix management belong to the consuming title.

## Evidence and limits

Imported from PS5_Proton `878c1e9`, retaining LGPL-2.1-or-later (Mesa remains MIT).
The prior payload graphics run proved four DXVK renderers, 96 submissions,
27 flips, pixels/input and 6/6 native PIDs absent. Prior WGL/Zink passed the
OpenGL 4.6 offscreen vertex/shader probe. Visible WGL presentation, native GL,
game-scale concurrency and a new console run of this extraction are unproved.
The owner has 32 slots; that capacity is not a claim of 32 Windows renderers.
Compatibility diagnostic variables currently retain PROTON_GPU_* spellings.
