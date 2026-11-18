# Graphics roadmap - current state to a playable Fable II

Ranked milestones. Each needs the listed evidence from a **hardware run**; a
change is not "done" until that evidence exists. Ordered so each one is
observable on its own.

## Where we are (after run 059, run-060 package prepared)

| blocker | class | state |
|---|---|---|
| `read of guest 0x00000000` in `sub_821C95D8`, ~48 s | guest VM (script argument) | argument 1 is nil; host-side work named the object it should be - the entity **`BowerstoneMarketExit_Gate` (`0xE6CCB784`)** from `data\levels.bnk`, resolved by name at runtime. run-059 hooks the engine's name-table lookup (`0x82C64018`) and logs key + result per call, which decides "table empty (mount never ran)" vs "populated, entry missing (registration)" |
| display | host renderer | **fence removal works**: run 059 presented 183 frames at `age=1`, i.e. the swapchain followed the guest at ~23 Hz. It then died at 9 s with **no crash block** (run-041 below-guest class) because command memory is rewritten while the GPU reads it - the old waits had been hiding that race. run-060 retires by submit age: 3 frame command slots, 16 present-copy slots, staging after 8 submits, no fence waits at all |

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
- Current: the queue does **not** die at the first texture upload - run 057
  recorded frames 0-127 after the upload copy's fence was declared lost, so
  `dkFenceWait` simply never observes these 2D-engine copies signalling, and the
  old 5 s bounded wait is what stalls the command processor per texture. Run-058
  switches the upload to the presenter's protocol: submit, never wait, poll the
  fence only to recycle staging. The presenter was already doing this
  (`branch=1`, swaps 1/2 show guest output).

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