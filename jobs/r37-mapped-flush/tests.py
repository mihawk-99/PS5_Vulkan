#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-test runner statuses from battery klogs, and a candidate/baseline diff.

  tests.py table KLOG...                 test -> status, one line each
  tests.py diff BASELINE_DIR CANDIDATE   compare two saved tables
"""
import json
import re
import sys


def statuses(paths):
    result = {}
    for path in paths:
        for line in open(path, errors='replace'):
            at = line.find('{"schema"')
            if at < 0:
                continue
            try:
                record = json.loads(line[at:].strip())
            except json.JSONDecodeError:
                continue
            if record.get('probe') == 'runner_test':
                result[record.get('detail', '?').split()[0]] = record.get('status')
    return result


if sys.argv[1] == 'table':
    for name, status in sorted(statuses(sys.argv[2:]).items()):
        print(f'{name} {status}')
else:
    base = dict(line.split() for line in open(sys.argv[2]))
    cand = dict(line.split() for line in open(sys.argv[3]))
    changed = [(n, base.get(n), cand.get(n)) for n in sorted(set(base) | set(cand))
               if base.get(n) != cand.get(n)]
    print(f'baseline {len(base)} tests, {sum(v == "PASS" for v in base.values())} PASS; '
          f'candidate {len(cand)} tests, {sum(v == "PASS" for v in cand.values())} PASS')
    for name, b, c in changed:
        print(f'  CHANGED {name}: baseline {b}, candidate {c}')
    print('identical' if not changed else f'{len(changed)} changed')
