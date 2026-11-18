# STANDING GOAL (user-set, durable)

Supersedes per-turn tasks until it is met. Linked from docs/STATUS.md.

GOAL: keep working continuously until Fable 2 runs as a WORKING SWITCH PORT.
F1 REAL FRAMES - the 2D copy lands and the presented image is no longer blank (end of the white/black screen).
F2 SURVIVAL - 60+ seconds continuous, no below-guest death at ~10 s.
F3 TITLE/MENU - the guest reaches the title or main menu with correct visible content.
F4 INPUT - a controller drives the menu.
F5 HARDWARE - the packaged build boots and renders on the real Switch.

## Status line (keep current)

**F1 OPEN** (copy PROVEN executing (068); guest-output surface PROVEN static white (069) -
the guest draws 1 static frame, never menu content; fault is upstream of the presenter.
Depth-format upload skip + staging cohorts in run-070 pending)
| **F2 OPEN** (run-072 CONFIRMED: GPU channel fault born at the kick of the frame carrying the
upload copies; OS kill ~9.4 s; short reads proven correct/closed; run-073: one-frame sampling
delay + depth images never bind + Full barrier w/ Image|Descriptors|L2 invalidate + probe off)
| **F3 OPEN** (guest stuck in boot/static-screen state; VM-nil crash ~50 s when survival allows)
| **F4 OPEN** (guest input works; menu not yet visible)
| **F5 PARTIAL** (packages boot on real Switch, screen static white) - last hardware run: 072.

## Autonomy contract (user-set)

- Do NOT end a turn at a checkpoint or a plan. When a step succeeds, immediately
  take the next step in the SAME turn.
- End a turn only when: (a) the user must run a package on hardware, (b) a hard
  blocker needs a user decision, or (c) the three-strike rule fires. In every one
  of those cases finish all host-side work first, so the next turn is pure
  execution.
- While waiting for a hardware run, keep working on anything that does not need
  the device: instrumentation, host-side validation, docs, next-milestone
  groundwork.
- Never sit idle waiting for permission already given. Standing approval: commit
  and push at verified checkpoints, messages stating the finding. Tags, releases
  and anything public still need approval.
- At each checkpoint reply (max 25 lines): what changed, evidence paths, F1-F5
  status, the next action already underway.
- Keep docs/STATUS.md and docs/GRAPHICS_ROADMAP.md current; this file carries the
  F1-F5 status line.

## Discipline (user-set)

- One change at a time, real-run verify, read the actual error before fixing, no
  boot regression.
- Do not re-litigate settled facts (fences signal on the render queue; not
  memory; not audio; boot is ungated) - recorded in docs/STATUS.md. If one is
  believed wrong, prove it with a new measurement before acting.
- Never claim a hardware result not seen. The user runs the Switch; ask for logs
  and diff them against docs/runs/.
- THREE-STRIKE RULE: if one defect survives three attempts, stop that thread,
  document the three attempts with evidence, and escalate - then continue on
  another thread if one exists.
