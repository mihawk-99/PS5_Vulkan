# R11 secondary recording — console PID 194

Probe: `b8-secondary` now records with `inheritance.framebuffer = VK_NULL_HANDLE`.
Queue: `jobs/r11-secondary/queue.txt`. Console: PPSA99988, PID 194, all three
cases PASS (235 PASS probe records, zero FAIL); title closed after capture.
The secondary triangle readback passes, all four C1 images present, and C4's
render-to-texture image and sampled square pass their pixel checks.

Driver archive: 14,383,172 bytes, SHA-256
`65550cae897ee2fab14224d07b7cf6766e986be21c9e5ba81359b0a0535c75ce`.
Two FTP reads of the deployed runner matched and contained the new recording
refusal string. Raw `Klog_Logs/r11-secondary.log` stays ignored; these twelve
JSON artifacts contain the extracted streams, regions and stages of PID 194.
No existing golden changed.

Reproduce the hardware capture after building/deploying the runner:

```sh
python3 tools/ps5_console.py battery PPSA99988 jobs/r11-secondary/queue.txt --output Klog_Logs/r11-secondary.log --timeout 240
python3 tools/ps5_console.py kill PPSA99988
python3 tools/golden.py extract Klog_Logs/r11-secondary.log golden/r11-secondary
```

The queue did not contain an AGC-level default-register anchor. The generic
`check-helpers` command does not accept driver-run metadata, and automatic
`golden.py replay` cannot find same-run defaults here. The measured host replay
uses the explicitly named existing `golden/b4/b4-headless-1.json` defaults;
all eleven new submissions/flip streams compare identically (host-replay.txt).
This checks the reused defaults against the new capture rather than labelling
them as same-run measurements. Reproduce that comparison:

```python
# Run from the repository root after tools/check-runner-cases.sh builds the host runner.
from pathlib import Path
import json, os, subprocess, sys
sys.path.insert(0, 'tools')
import golden
root = Path.cwd()
work = root / 'build/r11-replay'
work.mkdir(exist_ok=True)
run = json.loads(Path('golden/r11-secondary/run-1.json').read_text())
defaults = golden.register_defaults(json.loads(Path('golden/b4/b4-headless-1.json').read_text()))
for case in ('b8-secondary', 'c1-triangle', 'c4-rtt'):
    replay, queue, dump = [work / (case + suffix) for suffix in ('.replay', '.queue', '.dump')]
    replay.write_text(golden.driver_replay_text(run, defaults, case))
    queue.write_text('capture\n' + case + '\n')
    dump.unlink(missing_ok=True)
    with (work / (case + '.log')).open('w') as log:
        subprocess.run(['build/host/runner_host_driver', '--replay', str(replay), '--memory',
                        'free', '--cases', 'driver', '--app0', str(root), '--download0', str(work),
                        '--queue', str(queue)], stdout=log, stderr=log,
                       env={**os.environ, 'PS5_HOST_SUBMISSION_DUMP': str(dump)})
    # Host pixels are not a hardware witness; the submitted words are.
    subprocess.run(['python3', 'tools/golden.py', 'compare-run',
                    'golden/r11-secondary/run-1.json', str(dump), '--test', case], check=True)
```
