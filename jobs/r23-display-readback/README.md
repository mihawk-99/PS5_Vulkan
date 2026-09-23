# R23: swapchain image readback for vkQuake screenshots

Accepted on PS5 PID 229: 238 PASS, zero FAIL. TRANSFER_SRC is now reported
alongside COLOR_ATTACHMENT. The console probe copies each acquired swapchain
image into a host-visible buffer, compares every pixel with the independently
checked tiled render, and presents successfully. Four frames cover both buffers twice; the destination is poisoned
before each copy. The original c1-triangle path remains a regression.

The probe records the same present-to-transfer-to-present transitions as
vkQuake's upstream screenshot implementation. Unsupported TRANSFER_DST remains
a named refusal. No new tiling/layout or copy implementation is introduced.

Four copies each match 8,294,400 pixels. Sixteen draw/flip submissions replay
exactly. The generic replay's initial flip counter and an absent optional
helper workspace needed harness corrections; replay-notes.txt explains both
and retains the initial failures. Production commands and comparisons were
not relaxed. The guard only changes the no-workspace path used on the host.

Explicit driver build, host/cache checks, eleven gates, port five gates/scan
and template relink pass. After the harness correction, lint/unit/runner checks
and a console runner build pass again. Archive identity is in gates.txt.
Two deployed ELF reads and all five load segments match; console idle verified.
Reproduce: check.py Klog_Logs/<capture>.log, then replay.py from the root.
