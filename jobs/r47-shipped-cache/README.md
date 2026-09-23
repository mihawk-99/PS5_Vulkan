# R47: one cache directory per driver build, readable, so titles can ship it

Keys already include the driver build, so every build's entries were valid only
for it, and they piled up in one 0700 directory the console's FTP service could
not read (a 256 KiB directory file by now -- every driver build deployed during
R33-R46 left its full set behind). Now:

- entries live in `<base>/<first 16 hex of the build>/`, a self-contained set;
- the base and the build directory are 0777, because the FTP service is not the
  title's user and must read entries back and write shipped ones in;
- the base is `PS5VK_SHADER_CACHE_DIR`, or on the console the first line of
  `/app0/ps5vk-shader-cache-dir.txt` (a default-off test hook), or
  `/app0/ps5vk-shader-cache`.

Old entries in the base were left in place by this round; nothing reads them.
They were deleted afterwards (1,983 files, 11.1 MB; port evidence
m6-r49-kstuff-paused).

Host: build clean, check-shader-cache, NIR check.py, check-driver, 11 gates PASS.

Console (port evidence m6-r47-shipped-cache): a cold launch of build
`ece1bf911a93936a` compiled and stored 102 shaders; the port harvested all 102
(two reads each, header key checked against the file name); a launch pointed at
a fresh base holding only those 102 compiled nothing (0 compiles, 540 hits,
pipelines 0.09 s); the normal deploy then shipped them in the title (107 files,
every read-back ok) and the next launch compiled nothing.
