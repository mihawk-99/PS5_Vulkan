# R16 corrected mip tail — PS5 PID 207

277 PASS, zero FAIL; m2-solid, c7-mip-upload and r16-mip-blit all PASS.
Every one of four lower-mip frames matches all 8,294,400 pixels, with all
87,040 lower texels passing independent shifted-coordinate CPU checks.
Ten command streams replay exactly. Known benign unregister-busy warning;
title closed and console idle. See readback.txt, deployed-proof.txt and
host-replay.txt. Queue, source explanation, checks and reproduction are in
../../jobs/r16-mip-blit/README.md. Failed PID 206 evidence remains separately
in ../r16-mip-blit-before; no earlier golden was changed.
