# Agent instructions

This repository is a PS5 Vulkan compatibility probe. It records what the
console's GPU actually does, so that a Vulkan driver can be built on it.

## Read order

Read in this order and stop as soon as you have what you need:

1. This file. Frozen.
2. `docs/VULKAN_PROBE_PLAN.md` — the top-level plan: the gates, the milestone
   map, the invariants that constrain GPU-facing code, and the index of the
   reference files. About 150 lines. Read it once per session.
3. `docs/VULKAN_PROBE_ACTIVE.md` — the volatile state: current step, next
   actions, blockers, last verified runs. Read it last, before you start work.
4. On demand only, when the task needs it: `docs/PROBE_MILESTONES.md` (M1-M4
   canaries, runner, console tooling), `docs/M5_REFERENCE.md` (phase steps,
   workflow, version ladder, accelerator), `docs/HARDWARE_FINDINGS.md` (the
   evidence behind each invariant), `docs/M5_PHASE_A.md` / `_B.md` / `_C.md`
   (append-only run logs), `docs/NATIVE_TOOLING.md`, `docs/TESTING.md`,
   `docs/DEPLOYMENT.md`, `docs/PRESENTATION_ASSETS.md`,
   `docs/TROUBLESHOOTING.md`.

Never read a run log end to end to answer a status question. The active file
and each phase log's own summary say what passed.

## Volatility contract

| File | Rule |
| --- | --- |
| `AGENTS.md` | Frozen. Change only when the workflow itself changes. |
| `docs/VULKAN_PROBE_PLAN.md` | Static top-level plan. Edit only when a gate, a milestone summary or an invariant changes. Never record progress here, and keep it short: it is read every session. |
| `docs/VULKAN_PROBE_ACTIVE.md` | Volatile. Rewrite in place and keep it under about 120 lines. |
| `docs/M5_PHASE_*.md` | Append-only. Add dated entries at the end; never rewrite an existing section. |
| `docs/HARDWARE_FINDINGS.md` | Append-only. Add new findings at the end; never rewrite an existing one. |
| `docs/PROBE_MILESTONES.md`, `docs/M5_REFERENCE.md` | Stable reference. Edit when the specification changes. |
| Other `docs/*.md` | Stable. Normal edits. |

Progress, run results and "done" markers belong in the active file or a phase
log, never in the plan.

## Your task

The task in your prompt is the only task. `docs/VULKAN_PROBE_ACTIVE.md`
describes what the project is doing; it is context, not an assignment, and its
"Next" list is not a queue. When the prompt and the active file disagree about
what to work on, the prompt wins: do not start the active file's next step, and
say in your report that you noticed the difference.

## Prompt caching

The agent's context is cached by exact prefix, so only what sits *before* the
new material matters. Keep that part byte-identical between turns:

- Append, don't edit. Adding to the end of a file or a conversation is cheap;
  changing text that is already in context invalidates everything after it.
- Never put a date, version or "last updated" line at the top of a file that is
  read first. A date belongs in the volatile file, which is read last.
- Keep the read order above fixed. Reordering the same files changes the prefix
  and costs a full re-read.
- Do not reflow, renumber or bulk-rename. A reformat is a full re-read.
- Keep `docs/VULKAN_PROBE_ACTIVE.md` small: it is the only file expected to
  change every session, so its size is paid on every session.
- One fact, one home. Never copy status into the plan; link to the active file
  or a phase log instead.
- Batch documentation edits. One write-up at the end of a session beats
  continuous small edits, which invalidate the cache repeatedly.

## Build and verify

| Command | Purpose |
| --- | --- |
| `make` | Build the app into `dist/<TITLE_ID>/` |
| `make ci` | Reproduce the GitHub Actions build job on this host |
| `make lint` | Format, tidy, attribution, JSON, shell and asset checks |
| `make test` | Host unit and integration tests |
| `make inspect` | Static ELF/FSELF check of the built module |
| `make assets-deps` | Build the pinned BC7 encoder |
| `tools/check-driver.sh` | Driver: loader, direct and PS5-link runs, plus negative tests |
| `tools/check-psbc-link.sh` | Shader-compiler archive link check |
| `tools/check-vulkan-runtime.sh` | Vulkan runtime link check |
| `tools/build-all-titles.sh` | Every title builds with no warning and aligned segments |

The heavy Mesa-derived builds use ccache automatically when it is installed;
`PS5VK_DISABLE_CCACHE=1` opts out. Measured timings are in
`docs/NATIVE_TOOLING.md`.

## Console work

Deployment and klog capture are described in `docs/DEPLOYMENT.md`. Deploy with
`make deploy`, capture a run with `python3 tools/ps5_console.py klog`. Never
queue a test the plan marks as faulting: `kFaultingTests` keeps known GPU
faults out of the default queues for a reason.

## Working rules

- Commit finished, verified work; never push, and never rewrite published
  history.
- Keep the run logs factual: what was run, what it returned, what it proves.
- Do not add a non-PS5 toolchain flag to a script in `tools/`.
