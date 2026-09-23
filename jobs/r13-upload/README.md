# R13 upload metadata witness

Question: can a row-layout upload be recorded once per region, with the same
bytes and execution order, instead of once per row? The port's first visible
boot (PID 197) exhausted host memory in this recording path while map staging.
That is the observed failure; its full heap census is not measured.

The host C4 test's 64x36 upload originally creates 36 records of 272 bytes.
The candidate reuses the driver's region-copy representation, retaining source
and destination pitches, offsets and reversed-storage handling. Linear sides
copy one whole row at execution; tiled sides retain their measured run size.

Reproduce the metadata witness from the driver root:

```sh
bash tools/build-driver.sh
bash tools/check-driver.sh c4_texture
PS5VK_PROBES="$PWD/probes" PS5_HOST_REPLAY="$PWD/build/driver/check/c4-texture.replay" gdb -batch -x jobs/r13-upload/record-count.gdb build/driver/check/c4_texture_direct
```

The GDB witness stops at the actual upload, checks one record for one region,
then records 63 more valid uploads while the caller's source description is
still alive. It requires exactly 64 records and capacity below 32 KiB. It
quits without submitting those extra copies. The normal host C4 and format,
copy and mip tests independently verify submitted bytes and command streams.
`host-records.txt` records baseline and candidate measurements. This proves
metadata reduction, not that every source of the port's heap pressure is gone.

Console queue: m2-solid (same-run context defaults), c4-padded, c7-mip-upload,
c7-copy. Hardware and golden results will be recorded in golden/r13-upload.
