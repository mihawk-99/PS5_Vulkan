# R12 padded texture pitch — console PID 196

`c4-padded` samples a 32x36 RGBA8 image whose 128-byte source rows occupy
256-byte stored rows. Descriptor word 4 carries pitch-1 (63). Its nearest
frame must reproduce the pattern exactly; its bilinear frame uses the same
existing tolerance as `c4-texture`. Both cases passed both frames on the PS5.
The unchanged 64x36 case is in the same run. No older golden was changed.

Queue: `jobs/r12-pitch/queue.txt`; includes m2-solid for same-run register
defaults. PPSA99988 PID 196 completed: 224 PASS, zero FAIL; one known benign
VideoOut-busy warning during unregister, followed by title closure. Pixel
measurements and expectations are in `readback.txt` (JSON); four driver streams
replay exactly using this run's m2-solid defaults (`host-replay.txt`).

Driver archive: 14,385,036 bytes, SHA-256
`c37afdec4f7bc8fe107e18b5be21bd63fbf2231201d6e1aa16121a15966e89a2`.
The console converts SELF to ELF. Two reads were identical at 17,732,080 bytes,
SHA-256 `6ad80f7a362c09d15b37c7f9b3ba849ae90641bff0461b0e69dc2ab003191c47`.
All ELF headers and PT_LOAD bytes equal the local build/eboot.elf; differences
are only non-load trailing metadata. `deployed-proof.txt` lists segment hashes.
A whole-container comparison was initially rejected before launch; inspecting
the known transform resolved it without another deployment or console run.

Reproduce after building and deploying the runner:

```sh
python3 tools/ps5_console.py battery PPSA99988 jobs/r12-pitch/queue.txt --output Klog_Logs/r12-pitch.log --timeout 240
python3 tools/ps5_console.py kill PPSA99988
python3 tools/golden.py extract Klog_Logs/r12-pitch.log golden/r12-pitch
```

Host replay for each of c4-texture and c4-padded, after the runner-case gate:

```sh
case_name=c4-padded
mkdir -p build/r12-replay
python3 tools/golden.py replay golden/r12-pitch/run-1.json build/r12-replay/$case_name.replay --test "$case_name"
printf 'capture\n%s\n' "$case_name" > build/r12-replay/$case_name.queue
rm -f build/r12-replay/$case_name.dump
PS5_HOST_SUBMISSION_DUMP="$PWD/build/r12-replay/$case_name.dump" build/host/runner_host_driver --replay build/r12-replay/$case_name.replay --memory free --cases driver --app0 "$PWD" --download0 "$PWD/build/r12-replay" --queue build/r12-replay/$case_name.queue
python3 tools/golden.py compare-run golden/r12-pitch/run-1.json build/r12-replay/$case_name.dump --test "$case_name"
```

The host cannot render the pixel check. Its submitted command words are the
host witness; the PS5 measurements above are the rendering witness.
