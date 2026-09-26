# R82: 16-bit UNORM colour targets

LRPS2's hardware renderer keeps a colour-clip target in R16G16B16A16_UNORM and
refuses to start unless that format is a colour attachment. The driver sampled
and copied the 16-bit UNORM formats but did not render to them.

R16_UNORM, R16G16_UNORM and R16G16B16A16_UNORM are now colour attachments that
blend. The CB cannot blend a 16-bit normalized export, and a half-float export
rounds 16-bit values (11-bit mantissa), so each row exports 32-bit floats
whatever the blend state: 32_R for one channel, 32_ABGR for two and four, as
Mesa's ac_choose_spi_color_formats does for these formats once they blend.

The runner's v0-targets case gained the three rows, with values a half float
cannot hold (0x1235, 0x5679, 0x9abd of 65535) and a blended frame adding
0x0234, 0x0456, 0x0678 and 0x4000 to 0x1000, 0x2000, 0x3000 and 0x8000.

PS5 PID 171: all 19 target formats pass, solid and blended, every one of the
8,294,400 pixels of every readback exact (the three new rows included: 0x1235
solid, 0x1234 blended; 0x24561234 and 0xc0003678 for the four-channel blend).
v0-formats reports the three formats' optimal features as 0xdd81, sampled,
filtered, colour attachment, blended and both transfers.

Multisampled 16-bit UNORM targets are advertised by the driver's
format-independent sample-count rule (R75-R78) and have not been drawn by a
probe; nothing in LRPS2 asks for them.

Reproduce from the driver root:

    python3 jobs/r82-unorm16-targets/check.py jobs/r82-unorm16-targets/readback.txt

Console: build the driver and the runner, deploy while idle, then run
tools/ps5_console.py battery PPSA99988 with this queue.txt. The raw log is the
ignored Klog_Logs/r82-unorm16-targets.log.
