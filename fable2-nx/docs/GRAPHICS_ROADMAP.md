# Graphics roadmap - current state to a playable Fable II

Ranked milestones. Each needs the listed evidence from a **hardware run**; a
change is not "done" until that evidence exists. Ordered so each one is
observable on its own.

## Where we are (after run 069, run-070 package prepared)

Standing goal and F1-F5 status: [GOAL.md](../../GOAL.md).

| blocker | class | state |
|---|---|---|
| `read of guest 0x00000000` in `sub_821C95D8`, ~48 s | guest VM (script argument) | argument 1 is nil; host-side work named the object it should be - the entity **`BowerstoneMarketExit_Gate` (`0xE6CCB784`)** from `data\levels.bnk`, resolved by name at runtime. run-059 hooks the engine's name-table lookup (`0x82C64018`) and logs key + result per call, which decides "table empty (mount never ran)" vs "populated, entry missing (registration)". Not reached since 058: the ~10 s below-guest death fires first |
| ~10 s below-guest death (F2) | host / GPU channel | every run 059-067 dies at ~9-10 s during a `gui_textures.bnk` / `Globals` texture-bank read, no new crash block (`98A1A23D...` unchanged). Run-042 saw the Horizon fatal-error message: OS kill, not a guest fault. Candidates: command-memory reuse vs GPU read, a bad upload DMA once copies execute, or the first sampling draw |
| 2D copy (F1, half 1) | host renderer | **PROVEN on hardware (run 068): EXECUTED.** Descriptor A canonical (rowLength 4096 B, imageHeight 720 rows, Usage2DEngine tiled); waitIdle 5 ms, fence signalled, readback = the pattern. Mechanism: `submitCommands`/`signalFence` never kick the channel; only `presentImage`/`waitIdle`/explicit `flush` do - all earlier "fence never signals / copy fails" verdicts were unfetched submissions. Run-069 applies flush + bounded waits everywhere |
| blank screen (F1, half 2) | guest state (upstream of renderer) | run-069 PROVED the surface static white (fnv1a identical #30/#150, grid FFFFFFFF): presenter/copy innocent. Guest draws 1 static frame + 24 legitimate null draws (masked=0) - a boot/static-screen state, never menu content. Why the guest is stuck there is the next F1 target once survival allows |
| post-upload-batch death (F2) | host renderer | every run whose copies execute dies ~100-200 ms after the first batch is kicked. Batch led by xenos 23 = ZF32_X24S8 DEPTH copy (2D engine cannot target depth; RGBA8 proofs never covered it). Run-070: depth uploads skipped (named, opt-out) + staging pinned to fence-witnessed frame cohorts (never age). If it survives: RGB10A2 copies / first sampling draw are the next discriminators |
| display | host renderer | presents every guest frame (`[present] #N branch=1 age=1`, 180+/run, no `giving up on render recording`); present fence signals at #1. Screen still blank: no texture pixel has ever been proven in a GPU image, and rectangle-list UI geometry draws without textures |

M1 is the crash gate: the process cannot survive to the milestones below it. The
display item is not a separate milestone - it is the other half of the same work,
and run-059 measures whether the swapchain follows every guest frame.
M2 onwards cannot be verified until both close.

## M0 - Renderer survives a full session  [display half of run-056]

No GPU wedge, no OS kill for a hung channel, frames presented continuously.

- Acceptance: >5 minutes at the menu with no `fence wait timed out`, no
  `giving up on render recording`, no `present copy fence timed out`; fps stays
  at the 30 Hz cap.
- Evidence: `fable_2_0NN.log` (no timeout lines), `rex_perfil` with 10 s samples
  showing non-zero fps throughout.
- Current: the queue does **not** die at the first texture upload. The per-copy
  fence timeouts are a missing channel kickoff, not dead fences or a wedge:
  `submitCommands`/`signalFence` append GPFIFO entries without ringing the
  doorbell; only `presentImage` and `waitIdle` flush/kick (host-side binary
  finding, STATUS.md fence-verdict section). Uploads ride the frame list since
  run 066 and execute at the frame's present kick; run-067 proved the 960x720
  RGBA8 tiled copy harmless at setup. Run-068 (`waitIdle` witness, descriptor A,
  uncached readback) decides EXECUTED/DROPPED/WEDGED on hardware. M0 acceptance
  still stands: no timeout lines, no `giving up on render recording`, 30 Hz cap.

## M1 - Guest crash fixed, menu stays alive  [gates M2 onwards]

No guest access violation in the run; the process survives past the menu load and
ends only when the user quits. (The `0x24` page-set class this milestone was
written for is closed - see below.)

- Acceptance: no `Unhandled guest access violation` at all; the process ends only
  when the user quits.
- Evidence: run log without a violation, plus the crash report absence beside
  the NRO, and the function trace showing the call chain completing.
- Current: run-054 closed the page-set `0x24` crash class (all guards held at
  `sub_822659E8`, `sub_82426ED8`, `sub_822358A8`). The crash is now
  `read of guest 0x00000000` in `sub_821C95D8 + 0xD8` at ~52 s, and run-055
  showed the socket fix only moved it. Run-056 named the state exactly: VM
  **argument index 1 is nil** (`type = 0`, the same value the VM writes for an
  omitted parameter) where the binding wants a type-2 object, in a burst whose
  earlier calls carried a valid argument; the caller (`lr = 0x82228AF4`) is the
  VM **invoke helper** `0x82228830`, not game code, so the nil was already in the
  script's argument frame. The producer (which earlier call returned nothing) is
  what run-057 names, via an invoke ring plus callee and argument strings. M1
  stays **open/unverified**; no guard is applied, because a guard would hide
  whichever lookup is returning nil and the PC oracle proves that lookup is
  host-dependent.
- **Unifying hypothesis (strongly supported, not yet proven):** the nil argument
  and the missing `Data\Worlds\Albion\` registration look like one producer now.
  Host-side work (run-058, no launch) identified the missing argument as the
  **entity `BowerstoneMarketExit_Gate` (`0xE6CCB784`)**, found in the level
  entity manifest in `data\levels.bnk`; the text in the crash registers is that
  name, quoted, and it is resolved **by name at runtime** (no GUID or name
  reference exists in the scripts or the image). A GUI key for the same world
  (`Data\Worlds\Albion\`) also misses, so both symptoms sit on lookups into the
  Albion world. The fix still waits for the run-059 witness to say whether the
  entity table is empty (mount never ran) or populated but missing this entry
  (registration bug) - a guard or a blind producer patch would hide which.
- **P1 white screen:** guest output is attached (`branch=1`, swaps 1/2
  presented) but the queue wedges at ~16 s (`giving up on render recording`),
  after which every present copy times out. The M0 texture-upload stall is the
  blocker for all visual verification.

## M2 - Menu visible (untextured)

Rectangle-list draws reach the screen; UI geometry appears as flat placeholder
white/grey.

- Acceptance: menu panels and text boxes visible and correctly positioned;
  `frame N drew X` with X > 0 sustained; no `primitive_type` skips.
- Evidence: photo/screenshot of the screen plus the log's `first draw` line and
  per-frame draw counts.
- Requires: `kRectangleListAsTriangleStrip` variants in the pack, and a guarded
  fallback so a mismatched host vertex shader type is refused rather than drawn.

## M3 - Textures correct

Upload path replaced; sampled content matches the Windows build.

- Acceptance: menu art, fonts and icons look as they do on PC; no white surfaces
  where a texture belongs.
- Evidence: screen capture alongside the same scene on PC; per-texture upload log
  lines; no queue stall with `deko3d_texture_upload = true`.

## M4 - Entering the world / gameplay

Gameplay renders: world geometry, characters, shadows, HUD.

- Acceptance: can start a new game, see the first playable scene, move the
  character; no renderer-side crash.
- Evidence: several minutes of gameplay log with fps, plus capture.

## M5 - Audio and video completeness

All audio paths (music, speech, effects) and any FMV play correctly.

- Acceptance: no silent scenes, no missing streams; XMA statistics show decoded
  contexts.

## M6 - Stability and performance

Sustained 30 Hz in gameplay, no leak, no stall over a long session.

- Acceptance: a 30+ minute session in-game at the 30 Hz cap with stable memory;
  no timeouts, no frame-split warnings.
- Evidence: `rex_perfil` over the session with memory and fps trends.

## M7 - Full-game validation

Play through the campaign to completion; document any title-specific gaps.

- Acceptance: no progression-blocking rendering or emulation defect; known
  limitations listed in README.

## Standing risks

- Every remaining gate has failed at least one earlier hypothesis; prefer one
  measured variable per run over bundled guesses.
- GPU queue wedges on this hardware leave no error and can end the session, so
  any new GPU operation ships behind a switch until a run proves it safe.