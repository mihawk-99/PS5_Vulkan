#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay R22 captures exactly; optional positional arguments select cases.

Run from the repository root. Output goes to a fresh directory under build/.
Repeated-pass and subpass captures contain only selected frames; retain the
full host dump and compare exactly the frames the console actually captured.
"""
import os,sys,json,subprocess
from pathlib import Path
root=Path.cwd();golden=root/'golden/r22-target-flush/run-1.json'
run=json.loads(golden.read_text());import tempfile
work=Path(tempfile.mkdtemp(prefix='r22-replay-',dir=root/'build'));print(work,flush=True)
cases=['v0-two-passes','v0-subpass','c4-rtt','v0-query-full','v0-timestamp-driver','c8-resolve']
if len(sys.argv)>1:
 assert all(x in cases for x in sys.argv[1:]),sys.argv[1:]
 cases=sys.argv[1:]
results=[]
for case in cases:
 d=work/case;d.mkdir(exist_ok=True);q=d/'queue.txt';q.write_text(f'capture\n{case}\n');dump=d/'submission.dump';assert not dump.exists()
 with (d/'replay-build.log').open('w') as log:
  subprocess.run(['python3','tools/golden.py','replay',str(golden),str(d/'replay.txt'),'--test',case],stdout=log,stderr=subprocess.STDOUT,check=True)
 # This runner allocates its query-copy buffer before its counter pool.
 # The standalone query arm has no copy buffer. Both allocations have the
 # same size, and the generic replay lists the named pool first, swapping
 # their captured addresses here. Reorder only allocation metadata; every
 # command word and register table is still compared without tolerance.
 if case=='v0-query-full':
  replay=d/'replay.txt';parts=replay.read_text().splitlines()
  pools=[x for x in parts if x.startswith('region query_pool-')]
  assert len(pools)==1,pools
  replay.write_text('\n'.join([x for x in parts if x not in pools]+pools)+'\n')
 with (d/'host.log').open('w') as out, (d/'host.err').open('w') as err:
  subprocess.run(['build/host/runner_host_driver','--replay',str(d/'replay.txt'),'--memory','free','--cases','driver','--app0',str(root),'--download0',str(d),'--queue',str(q)],env={**os.environ,'PS5_HOST_SUBMISSION_DUMP':str(dump)},stdout=out,stderr=err,check=True)
 lines=dump.read_text().splitlines();wanted=[x for x in run['submissions'] if x['test']==case]
 if case=='v0-two-passes':
  assert len(lines)==160 and len(wanted)==4,(case,len(lines),len(wanted))
  chosen=[lines[i] for i in [0,40,80,81]]
 elif case=='v0-subpass':
  assert len(lines)==4 and len(wanted)==2,(case,len(lines),len(wanted))
  chosen=lines[:2]
 else:chosen=lines
 selected=d/'captured-frames.dump';selected.write_text('\n'.join(chosen)+'\n')
 p=subprocess.run(['python3','tools/golden.py','compare-run',str(golden),str(selected),'--test',case],capture_output=True,text=True)
 (d/'compare.txt').write_text(p.stdout+p.stderr);print(p.stdout,flush=True);results.append(p.returncode)
print('All host submissions retained. Repeated-pass capture selects the first of each 40-frame shape; subpass selects frame zero only.',flush=True)
raise SystemExit(1 if any(results) else 0)
