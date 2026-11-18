**Standing goal: [GOAL.md](../../GOAL.md)** - F1 real frames, F2 survival, F3
title/menu, F4 input, F5 hardware; status line kept current there. The autonomy
contract and discipline in it govern every session.

# HANDOFF TO FRESH SESSION - 2026-11-18

## What this is

A Nintendo Switch port of **Fable II** (Xbox 360), built on ReXGlue static
recompilation with a native **deko3d** renderer. The guest runs: JIT/recompiled
code, audio, saves, input, file loading, the frame loop and the presenter all work,
and the menu is reached. Proven on hardware this block of runs: the guest renders
and the presenter follows every guest frame (`[present] #N ... age=0/1`, 180+
frames), rectangle-list UI geometry is now drawn (0 `primitive_type` skips, was
194), audio is healthy (48 kHz), memory is healthy (2277 MiB free after GPU setup),
and the socket/BSD layer works. **Not proven:** any texture pixel has ever reached
a GPU image, and the app dies ~9-10 s in with a below-guest death that writes no
crash block (identical `rex_crash.txt` hash `98A1A23D...` every time, so it carries
no fault information).

## Fence verdict as it stands (do NOT restate "fences never signal")

- The **present/frame fence signals** on the render queue: `[present-fence]
  SIGNALLED ... at present #1 (1 frame after the copy was submitted)`, repeatedly,
  in runs 062-067.
- **Per-copy fences time out** (250 ms and 1000 ms bounds) while the same queue
  keeps presenting frames - i.e. a submission that reports nothing while
  everything after it runs. That is *not* proof of a wedge and *not* proof that the
  copy ran.
- The **2D copy chain reads back zeros** end to end (run 064: `src=5A rb=00`,
  checksum 08D4A082), and a plain **buffer->buffer** copy plus a **clear** also
  failed with the destination still holding its `0xAA` pre-fill (run 065).
- Fences DO work on this stack (run 062). The earlier "fences never signal" note
  was wrong and has been deleted from the code; every wait stays bounded and named.
- **Named mechanism for the per-copy timeouts (host-side, 2026-11-18, unverified
  on hardware until run 068):** disassembly of the installed `libdeko3d.a`
  (`dk_queue.o`, `dk_fence.o`) and libnx `gpu_channel.o` - byte-identical (MD5) to
  the WSL toolchain the NRO links - shows `submitCommands` and `signalFence` only
  append GPFIFO entries to a 2048-entry batch; **neither kicks the channel**. The
  doorbell (`nvGpuChannelKickoff`) fires only from `Queue::flush`, reached in the
  frame loop via `presentImage`, and from `waitIdle` (= signalFence + flush +
  wait). So every standalone copy probe (058 boot, 065 a-d, 067 bisect) timed out
  with the entries never fetched: 065's copy-diag ran on the presenting thread,
  and its four 250 ms waits ARE the present #179 (03:33:19.016) -> #180
  (03:33:20.312) gap; #180's kick then executed the whole burst, which is why the
  pre-fills survived every readback. Fences are a monotonic per-channel semaphore
  (`wait` = `*payload >= expected`), so a signalling present fence retroactively
  satisfies every earlier fence on queue 0x1 - the 067 copy fence would read
  SIGNALLED if re-polled after present #1 (it never was). This retracts "fence
  primitive dead" (058) and "the standalone submit path never reaches the GPU"
  (065) as hardware claims: both are the same missing-kickoff artifact. Run-068's
  `waitIdle` witness flushes/kicks, so its verdict is trustworthy.
- **RUN-068 HARDWARE PROOF (2026-11-18, `docs/runs/068/`):** the copy EXECUTES and
  lands the pattern. `[copy-sync] A rows: dkQueueWaitIdle RETURNED after 5 ms`,
  `fence state after WaitIdle = signalled (queue error state: no)`, readback
  `first pixels [5A5A5A5A 5A5A5A5A]`, `verdict EXECUTED`. Descriptor A is
  canonical: rowLength 4096 BYTES, imageHeight 720 ROWS, flags 0x800
  (Usage2DEngine), tiled. The 2D engine, the descriptor and the fence are all
  fine - the submission just needed to be kicked/waited properly. "The copy is
  the killer" is DEAD. The ~9.5 s below-guest death is UNCHANGED (180 presents,
  gui_textures.bnk read, crash block `98A1A23D` identical) and is a separate
  defect (F2), not chased ahead of F1.

## The open question (F1) - the copy question is ANSWERED

Run 068: **EXECUTED** - the 2D copy lands the pattern. Run 069 answered the
blank-screen question: the guest output surface (1280x2048 host target) is
**STATIC white** - fnv1a identical at present 30 and 150, grid all FFFFFFFF.
The presenter and the copy path are innocent; the guest itself draws one static
frame (drew 1, skipped 24 legitimate null draws) and never advances to menu
content. The fault is UPSTREAM: the guest is stuck in a boot/static-screen
state, and every run dies ~100-200 ms after the first texture-upload batch is
kicked. Run-069's batch was led by a **ZF32_X24S8 depth-format copy** (xenos 23)
that the RGBA8-only proofs never covered - prime suspect: depth is not a
supported 2D-copy destination on Tegra (hypothesis; run 070 measures it).
Run 070 skips depth-format uploads (named, opt-out) and pins staging to
fence-witnessed frame cohorts; acceptance is 60 s survival + the upload probe's
pixels + differing guest-readback hashes.

## Exact next action

Build (WSL), stage, then the user runs it on the console:

```powershell
wsl bash -lc "cd /mnt/h/Projects/Fable2Port/Fable-2-Recomp-nx/fable2-nx && DEVKITPRO=/opt/devkitpro tools/build_switch.sh --nro"
powershell -ExecutionPolicy Bypass -File H:\Projects\Fable2Port\Fable-2-Recomp-nx\fable2-nx\tools\refresh_stage.ps1
(Get-FileHash "H:\Projects\Fable2Port\stage\switch\Fable II\fable_2.nro" -Algorithm SHA256).Hash
```

The staged package is `H:\Projects\Fable2Port\stage\switch\Fable II`
(`fable_2.nro`, `fable_2.toml`, `fable2_config.toml`; run-070 NRO hash in
`docs/RUN_070.md`). The user copies the three files to `E:\switch\Fable II`,
launches full-memory, and returns the **whole `logs/` folder as-is** (never
renamed) plus a photo. Read in `fable_2_0NN.log`:
- `[upload] SKIPPED depth/stencil format` - the ZF32_X24S8 copy that led every
  fatal batch is now skipped (placeholder); the batch becomes RGBA8 + RGB10A2;
- `[frame] submit #N carried U upload copy/copies` + `[staging] released ... on
  the present-fence witness` - staging lifetime is fence-witnessed, never age;
- `[upload] frame-path readback (presenter poll) ... grid=[...]` - proof the
  in-frame upload landed (now the first NON-depth upload, RGBA8 480x360);
- `[guest-readback] #30/#150` - after the fix the fnv1a SHOULD differ (the guest
  animates once it gets past the boot/static state);
- `[death-watch]` / `[skip-census]` - kept; the death must not recur within 60 s.
If the death survives the depth skip, the next discriminators are the RGB10A2
copies and the first draw that samples an uploaded image (one cvar at a time).

## Evidence

- Run records + logs: `docs/runs/058/` .. `docs/runs/068/` (logs are `*.log` and
  intentionally git-ignored; the `RUN_0NN.md` write-ups are tracked).
- `docs/GRAPHICS_ROADMAP.md` - milestones; the "Where we are" table is current.
- This file - run log table, per-run findings, next actions, licence and the
  parked Vulkan/NVK note.

## Three traps that cost the most time (do not repeat)

1. **Generalising "fences never signal" from one boot-time probe.** It justified
   deleting every wait, which removed the only pacing the pipeline had, and
   produced a 9 s host death and (in run 061) a boot hang. The fence works; only
   some *submissions* never report.
2. **Blocking the boot path on a diagnostic** (`dkQueueWaitIdle` inside the copy
   validation, run 061): black screen, live process, one wasted run. Diagnostics
   must never gate boot and must never block unbounded.
3. **Trusting a timeout instead of reading destination bytes.** Four packages went
   by without a single pixel. The discriminator design - known content on both
   sides, a pre-filled readback, and an explicit EXECUTED/DROPPED/WEDGED verdict -
   is what finally produced usable evidence, and it is the pattern to keep.

# Fable II (Switch) - status

Single source of truth for where the port stands. Hardware results are the user's
runs; anything not yet run on the console is marked **unverified**.

**Standing goal: [GOAL.md](../../GOAL.md)** - F1 real frames, F2 survival, F3
title/menu, F4 input, F5 hardware. Its status line is kept current there; the
autonomy contract and discipline in it govern every session.

## Log return protocol - NO RENAMES, EVER (user directive)

The user will not rename or delete any file. Every package instructs: **copy the
whole logs folder as-is**. Detect new crash blocks by hashing
`rex_crash.log` and comparing with the copy archived from the previous run:
unchanged hash = no new crash (a valid result); changed = parse the **newest
block** (blocks append at the end). Never ask for renames
(`rex_crash.preNNN.log` is obsolete).

## PAUSE ON PC TEST LAUNCHES (hub directive, 2026-11-16) - ACTIVE

The user is on the machine: **no test windows, no audio, no launches of any kind
on this PC** until the hub lifts the pause. This sits on top of the visible-window
order below (which still governs how runs are done once the pause is lifted).

- Do **not** launch Ryujinx, the PC oracle (`fable_2.exe`), launchers, the NRO or
  any game executable; do not start anything with a window.
- Do not hide anything either - the point is "nothing runs", not "nothing visible".
- Host-side work is allowed and expected: code, static analysis, builds (they are
  console/codegen only), docs, staging the SD folder.
- Hardware runs are still the user's on their console; nothing here runs them.
- Blocked next actions are recorded under "Next actions" with the exact launch
  that is waiting on the pause.

## Testing protocol - ordinary visible windows (user, 2026-11-16)

Any Windows GUI run (the `fable_2.exe` oracle or a reference build) is launched
**normally**, in a visible window:

```
powershell -Command "& '<full path to fable_2.exe>' <args>"
```

- **Do not** use `run-hidden.ps1` or any hidden desktop; do not minimize, move,
  hide or kill the window; no window-guard scripts and no daemons that suppress
  windows. (The old hidden-desktop protocol and its `fable_2` window guard are
  retired with the quiet-mode order below.)
- Set the PC oracle's `fullscreen = false` before a run so it opens as a window
  rather than taking the screen - that is a config change for the app, not
  window suppression.
- Console builds, codegen and file work are unchanged.

## VISIBLE WINDOWS - STANDING ORDER, PERSISTENT (user, 2026-11-16)

**QUIET MODE IS LIFTED.** The user ordered everything unhidden. This supersedes
the 2026-11-15 quiet-mode order in every respect.

- Do **not** use `H:\Orchestration\tools\run-hidden.ps1` (hidden desktop). Launch
  apps normally, as **visible** windows on the user's desktop.
- Do **not** minimize, move, hide or kill a test app's window. No window
  suppression scripts, no watchdog daemons.
- Test runs are ordinary visible windows. If that is disruptive, the **user**
  says so - it is their call, not the agent's.
- Everything else stands: no dialogs/message boxes/UAC if avoidable, console
  builds are fine, hardware packages are prepared and the user runs them (never
  attempt a hardware run), and no commits or releases without approval.
- Persist this for every future session; it replaces the quiet-mode section that
  used to live here.

Last hardware run: **069** (2026-11-18, guest-output surface proven STATIC
white - fnv1a identical at #30/#150, presenter/copy innocent; one real draw per
frame, all 24 skips legitimate; death ~100-200 ms after the first upload batch
is kicked; upload #1 is a ZF32_X24S8 DEPTH copy the RGBA8 proofs never covered).
Next: run-070 (**staging pinned to frame cohorts, released only on a later
frame's present-fence witness - never on age; depth/stencil-format uploads
skipped behind deko3d_upload_skip_depth=true, the death discriminator; probe
4x4 grid; per-frame upload-copy count at submit**) - built and staged, NRO hash
in `docs/RUN_070.md`, tag `m1-f1-070`, unverified.

**Run-067 result, in one line:** the copy-only bisect logged `copy fence TIMEOUT
after up to 1000 ms` for the tiled destination (flags 0x800, size 2949120, align
512, derived pitch 4096 = the rowLength passed) and rejected the pitch-linear
destination (size 0), but the same queue then presented 180 frames with the present
fence signalling at #1 - so "the copy is the killer" is **not** supported: a
submission that times out while everything after it runs looks like a submission
that never executed. Run-068 stops trusting the fence: it submits one copy and asks
`dkQueueWaitIdle`, logging the fence before/after and reading pixels.

**Run-056 result:** the crash is a **nil object at VM argument index 1**
(`type = 0`) in a burst of otherwise valid calls, and the *caller* is the VM's
invoke helper at guest `0x82228830` (not game code), so the nil was already in
the script's argument frame. Detail: `docs/runs/056/` and `docs/RUN_057.md`.

**Run-056 evidence used (parsed, not guessed):** `docs/runs/055/rex_crash.log` is
`docs/runs/054/rex_crash.log` byte-for-byte with **one block appended** (054
`A11477CCâ€¦`, 55576 bytes; 055 `DBB147D7â€¦`, 58946 bytes; prefix identical), and
the appended block is the `read of guest 0x0`:
`pc imagen+0x96B688 = sub_821C95D8+0xD8`, instruction `0xB8606A80`
(`ldr w0,[x20,x0]`, x20 = guest base, x0 = 0). `addr2line` on `fable_2.elf`
puts that pc at `port/generated/default/fable_2_recomp.21.cpp:370`:

```c
ctx.lr = 0x821C961C;
ProcessAndProcessAndProcess206_822281F8(ctx, base);  // VM argument fetch
ctx.r3.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);           // <- the fault: fetch returned NULL
```

`sub_822281F8` is `element = AccessElement8_82A1E620(vm, index); type = [element+4];
type==2 -> [element]; type==7 -> [element]+24; else -> 0`, so **the argument's VM
type tag is neither 2 nor 7** - a script value of the wrong kind (or an unwritten
slot) where the opcode handler expects an object. The same record holds
`x2 = 0x4F7C3A78` (guest heap) and `x12/x13 = "arketExit_Gate'."` (x12 low-to-high is "arketExi"), i.e. a script
symbol name, in the faulting thread's registers. Run-056 named the index and the
tag; run-057 names the script call.

Full log history (001-040) at `docs/runs/history/`; runs 041-044 at
`docs/runs/041/`, `docs/runs/042/`, `docs/runs/043/`, `docs/runs/044/`.

## Current state

Works on hardware:
- Native deko3d backend, 512 MB guest mirror, host render targets, native present.
- Guest ring, JIT, audio, saves, input, file loading.
- Shader pack loads (28 shaders, 56 texture bindings) and DKSH binds.
- Frame loop confirmed: `swap 0/1/2 ... presented`, guest output reaches the
  display, `first draw` issued.
- Menu runs at the guest's 30 Hz cap (29.6-29.9 fps) with GPU Commands ~2.5% CPU.

Does not work yet:
- No menu pixels. Every draw in a menu frame is either **empty** (no pixel
  shader, `RB_COLOR_MASK` 0, depth/stencil off - 24/frame, Xenia skips these too)
  or a **rectangle list** (`host primitive 8`, 1/frame), which deko3d has no
  primitive for.
- Texture uploads stall the display, and it is the **wait**, not the copy: the
  copy's fence never signals on this stack (run 057: 127 frames advanced after
  the fence was declared lost), and the 5 s bounded wait then blocks the command
  processor for five seconds per texture. Run-058 gives the upload the
  presenter's protocol - submit, never wait, poll the fence only to recycle
  staging (`deko3d_upload_no_wait`). Earlier exclusions (block alignment, copy
  shape, 4096-byte pitch, plain RGBA8) still stand; the boot probe now starts
  with fence-only and clear-only controls so a broken fence can never be read as
  a broken copy again.
- Guest VM null (the current crash, **not** the closed page-set class): the
  script/VM opcode handler `sub_821C95D8` fetches an argument, receives NULL
  because the VM slot's type tag is neither 2 nor 7, and loads through it -
  `read of guest 0x00000000` at ~47-52 s. Run-056 showed the slot is **argument
  1 = nil** (type 0) in a burst of otherwise valid calls, and that the caller is
  the VM invoke helper `0x82228830`, so the nil came from the script's own
  argument frame. **Host-side work named it**: the object should be the entity
  `BowerstoneMarketExit_Gate` (`0xE6CCB784`, from the manifest in
  `data\levels.bnk`) - the quoted name is what the crash registers hold - so the
  by-name entity lookup returned nil. See "Host-side finding" below.

## Gates (ordered)

1. **Guest VM null** (`read of guest 0x0` in `sub_821C95D8`) - blocks everything;
   fires ~50 s in, after the menu has drawn. Argument 1 is nil at the binding
   (run 056); the producer is not named yet. The page-set `0x24` class stays
   closed (guards at `sub_822659E8`, `sub_82426ED8`, `sub_822358A8`).
2. **Texture upload copy shape** - the block was never the fence; it is
   `DkCopyBuf.imageHeight` carrying the total byte size instead of the row count,
   which makes every upload an invalid DMA the moment it executes (run 060 died at
   the upload with no crash block). Run-061 fixes the units and proves one 64x64
   round trip byte-for-byte at boot before any real texture.
3. **Rectangle lists** - the only draw in a menu frame that could produce UI
   pixels; needs expanded VS variants in the pack.
4. **Texture upload** - replace the 2D-engine copy (staging image + blit, or a
   DKSH pass from the mirror).
5. Correct rendering, then world entry, then full-game validation.

## Run log

| run | build | result |
|-----|-------|--------|
| 034 | fb30662e | 1.8 -> 0.0 fps; queue wedged at first texture upload (960x720 RGBA8, 3840-byte rows); console needed a power cycle |
| 035 | fb30662e | ~70 s; renderer stopped inside 20 s; OS killed the process for the hung GPU channel; log tail lost (skip reports rate-limited) |
| 036 | 17c85610 | Frames 0-2 presented (placeholder then guest output); wedge again at the same upload, and `WaitForPendingList` showed the preceding render list was fine, so the copy itself is guilty; run ended on the guest 0x24 crash |
| 037 | 41c2a4fd | Same upload wedged with rowLength padded to 4096, excluding pitch |
| 038 | 29b616e8 | **No wedge** with uploads off; frames presented, `first draw` issued, 0 draws again (24 empty + 1 rectangle list); log stops at 8 s in the GUI texture load. **No crash block** - the crash records attributed to 038 are from 036/037. |
| 039 | 0B0D5CBF | **no crash reached**: log spans 9 s; `rex_crash.log` unchanged. Stall, not fault. |
| 040 | 0B0D5CBF | Same; the archived `rex_crash.log` is byte-identical to 039's (`C0B184D2...`) - neither appended a block. |
| 041 | 704867F4 | Build tag confirmed. **The app died by itself at 9 s** (user observation): no crash block, no violation, no witness - a below-guest death, not the null-deref. Runs with deko3d + uploads disabled (038-041) all die at 8-9 s. |
| 042 | B14BC4F7 | **draws exonerated; OS kill confirmed.** Tag confirmed, `diagnostic_skip_draws` took effect (frames advanced to 127, `drew 0`), the app still died at the same ~9 s terminus, and the user saw the Horizon message **"The software was closed because an error occurred"** - a fatal host fault/OS kill, not a clean exit. No witness, no violation, no `[gpu-wait]`, crash file still stale. |
| 043 | 87D32069 | **M1 provenance captured.** Uploads enabled: the 5 s upload timeout fired, the app survived to 43 s and went beyond character selection, then the witness fired - `sub_822358A8 r3=0 lr=82390590 r4=425DDB90`, chain `h=42538C10, i=0` - and the guest violation followed. New crash block (`3C136827...`). The 9 s no-upload death is a separate defect. |
| 044 | F328F67B | **class named.** `h=42538C10` is a **CECPlayerControl** (type descriptor `820A687C` = deleting destructor `8234E398` + name; ctor `sub_8234E2B0` zeroes `+8`/`+0x64`; writer `sub_8234EC08` called from update loop `sub_825F3050` @ `825F3538`). `+8` and `+0x64` null = instance not yet updated on this path. |
| 045 | 5EC15850 | **writer never ran.** Tag confirmed, witness fired at 37 s with the same `h`/`+0x64=0`, but **zero** `[gui-writer]` lines: `sub_8234EC08` was never called. The gate is the HUD state machine (below). |
| 046 | DA31C44B | `[gui-state]` shows state 1â†’2â†’3â†’5 with **flag=1**; case 2 and its flag were reached, yet **no writer**: the update call path is bypassed or targets another object. Witness at 39 s, same `h`, new crash block. |
| 047 | D7D6163D | **two instances.** `[gui-upd] self=42220210` (the update target); the draw faults on `42538C10`, reached as `[[42220210+0x24]+4]`. The updated object is in the witness chain; the faulting one is never updated. |
| 048 | B6D7909E | **pass and update are not coupled.** `[gui-pass]` ran once at 16:09:58 with `self=404BC8A0`; the faulting update ran at 16:10:02 on `42220210` with `w36=40531DC8 idB4=404CF618`, `n24=40531DC8 h2=42538C10 h2+64=0`, chains matching the witness. The pass call's target is `[component+4]` (=404BC8A0), so the pass is a manager-level pass; the later update took a branch that never reaches the pass call at `0x8233F064`. |
| 049 | F2EB4C2E | **pass path confirmed.** `[gui-mark] pass` at 22.413 (`self=42220210`, `h2=42538C10`, `loop=0`) was the only mark - `wait` never fired; the `[gui-pass]` followed 4 ms later and the update returned 4.1 s after that with `h2+64=0`. The wait loop and the state machine are exonerated; the empty/not-ready UI batch inside the pass is the gate. New crash block `270D8442...`. |
| 050 | 837831EF | **root-cause candidate named.** `[gui-batch] bit ret=0` (key template `Data\Worlds\`, verified byte-exact at `0x820A7810`), `sub_825F45A8 -> 0`, all twelve batch vectors empty, `loc_825F389C` exit, gate probe silent. The lookup that decides the batch misses; the writer never runs. New crash block `32D5C1D1...`. |
| 051 | 90996B1B | **key named.** `[gui-key]` shows the template `Data\Worlds\` and a composed string containing `ds\Albion\` -> the registry key is `Data\Worlds\Albion\`; builder takes **2 ms**, so the 4.0 s is in the pass setup, not the key. Lookup still misses. Crash block `077DC955...`. |
| 052 | 66DAF6DB | **guard too narrow.** `[gui-guard] #1` skipped `sub_822358A8` at 53.964; 3 ms later the same null page set faulted from `sub_82426ED8`'s rect/blit path (`r4 = 0+36 = 0x24`). `[gui-phase]`: `82A492B0` alone ~2.05 s, ~2.2 s in the gap after it. Crash block `E5085770...`. |
| 053 | 2AC30C2B | **third consumer.** Both guards fired (`sub_822358A8`, `sub_82426ED8 element #1`), crash still 3 ms later at the same instruction via `sub_822659E8` (~75 callers). Crash block `C404B2DF...`. White screen traced to the presenter: on copy-fence timeout it returned `kNotPresented`, so `presentImage` never ran after frame 0. |
| 054 | 866810DF | **page-set class closed.** All guards held (`sub_822358A8` #1/#2, element #1, clip #1-#3); `read of guest 0x24` never recurred. Crash moved to a new class at ~50 s: `read of guest 0x00000000` in `sub_821C95D8+0xD8`, right after `[svr2011] net: socket(2, 1, 0) failed: 10022`. Presenter: `branch=4` -> `branch=1 target=1280x2048 guest=1280x720`, swaps 1/2 presented, then `giving up on render recording` at ~16 s. Crash block `A11477CC...`; its appended record is `imagen+0x96b268`, instruction `0xb8606a80`, `x21=0x821C95F0` - run-056 pins that record's real site. |
| 055 | F3E07E78 | **socket fixed, crash unchanged.** `[socket] Horizon BSD service initialised` logged and `socket(2,1,0) failed 10022` gone, but the fault is **the same site as 054**: `sub_821C95D8+0xD8`, instruction `0xb8606a80`, `far=0`, `x0=0`, now at ~52 s. The block 055 appended is `imagen+0x96b688` = `sub_821C95D8+0xD8`, which `addr2line` resolves to the VM argument fetch returning NULL (see the top of this file). The socket error was a timing coincidence, not the cause. Present path unchanged (no upload fix in this build). |
| 056 | C2040A46 | **VM argument captured; upload probe silent.** `[vm-null] index=1 type=00000000` - the first argument is **nil** (VM type 0) where the binding wants a type-2 object; the same binding ran twice in the same burst with a valid `type=7` argument first, so the nil is one entry of a call run. Caller `lr=82228AF4` = a `bctrl` inside the VM invoke helper `0x82228830` (prototype at `[callee+16]`, param counts `+73/+74/+75`, missing params filled with type 0, dispatch via `[[vm+20]+4]->[+0]->vtable[+16]`), so the nil was already in the script's argument frame - not produced at the call site. Upload: **no `[upload-probe]` line at all**; `deko3d_texture_upload` did take effect, so the toml was read but the probe key was not in it and the cvar defaulted to false with nothing logging the gate. Crash block `910C0FBD...` (055 is an exact prefix; the appended record is the same site again, `imagen+0x96dd08` in that build). |
| 057 | A4B7DDC9 | **the fence is the problem, not the copy.** Probe ran: `gate: cvar deko3d_upload_probe=true`, then `control-presenter-flags: TIMEOUT in 2000 ms` (queue error state 0) and - old design flaw - every other variant skipped. But the renderer kept going: `frame 0` 23:56:58.386 -> `frame 127` 23:57:03.524, so the 64x64 copy did **not** stop the queue. The 5 s wait for the real upload at 23:57:05 then stalled the command processor for 5 s, and its fence timed out too. `dkFenceWait` never observes these 2D-engine copies signalling. VM names: `[vm-name]` printed `'PP.4......'` / `'CI32Vector2'` / `'sh...'` - the text is not at `[value]`/`[value+16]`, so that offset was a guess. |
| 058 | E3E9E4CA | **fence primitive dead; upload fix works.** `fence-only: TIMEOUT in 1000 ms` and `clear-only: TIMEOUT` - a signal on an EMPTY list times out, so `dkFenceWait` never signals at all. `deko3d: uploaded texture 2D (960x720, format 23) without a fence wait; 1 staging block(s) in flight` confirms the no-wait upload. Then `fence wait timed out for previous frame's render list after 250 ms` and `giving up on render recording` 3 ms later: the resolve/render-list poll was the last blocker and it *stopped recording*. VM callee-object extraction still garbage (`words 50500034 06010100 ...`) - the name is not at those offsets, so that approach is dropped in favour of the name-table lookup. |
| 059 | F37E0C57 | **display worked, then a below-guest death.** Fence removal active and logging (`previous frame's render list fence not signalled on the first poll; continuing without a wait`); upload no-wait active (`uploaded texture 2D (960x720, format 23) without a fence wait`). Presents ran to **#183** (`[present] #183 slot=2 branch=1 guest#182 age=1 since_refresh=0ms`) at ~23 Hz - the swapchain followed the guest frames. Then the log ends at 23:06:29.471 on a `gui_textures.bnk` read with **no guest violation and no new crash block** (`rex_crash.log` byte-identical to 058, SHA-256 `98A1A23D...`): the run-041 below-guest class. Cause: presents are no longer throttled by the old waits, so command memory (present-copy lists: 3 slots keyed by swapchain image; the 2 MB frame list: one block per frame) is rewritten while the GPU is still reading it. No `[vm-lookup]` line because the GUI batch runs at ~22-50 s and the run died at 9 s. |
| 060 | CCA51419 | **display path fixed, upload copy invalid.** `[present] #1..#7 branch=1 guest#N age=1 since_refresh=0-3 ms` at ~50-100 ms intervals and no `giving up on render recording`: the swapchain follows every guest frame. Then the log ends at ~10 s on a `gui_textures.bnk` read with **no guest violation and no new crash block** (`98A1A23D...`) - the upload itself is fatal now that it executes. Host analysis found the cause: `DkCopyBuf.imageHeight` was given the total byte size (2,949,120) where deko3d wants the row count (720), so the 2D engine walked past the block -> invalid DMA -> GPU fault -> OS kill. Earlier runs only survived because the timed-out fence skipped the copy. |
| 061 | BE1877EA | **REGRESSION - boot hung, black screen.** Units fix is in, but the boot copy validation ended in `device_.WaitIdle()` on a queue that never completed its list: log stops at `Initializing shader storage for title 4D5307F1...` (1.18 s, 5.2 KB), zero presents, no crash (`rex_crash.log` byte-identical to 060). `[gpu-validate] step0 empty-list fence: TIMEOUT in 250 ms` on the **first submission, before any copy** rules out a poisoned queue but does not prove fences are broken - the queue had never executed anything; 060 proved it does. |
| 062 | 7DA12455 | **fences work.** `[present-fence] SIGNALLED on the render queue at present #1 (1 frame after the copy)` - the primitive is fine and the old note is retracted. 186 presents over ~9.5 s, then the same below-guest death as 060/061 (crash block unchanged, `98A1A23D...`) - now blamed on age-based staging retirement. Draws: 552 `no_render_targets` (mostly legitimate null draws), 194 `primitive_type` = `kRectangleList` (xenos 0x08, **not** a quad strip - `kQuadList` is 0x0D and was already mapped), 3 `no_rasterization`. Also 185 EDRAM-transfer lines and a pending depth-resolve copy; 70 slow 2 KB reads (~1.3 s). |
| 063 | C5E37F3B | **rectangle lists fixed; copy fences still silent.** `[primitive] rectangle list #1..#8 drawn as a triangle strip` and `primitive_type` skips 194 -> **0** - that class is closed. 180 presents in ~10.3 s, present fence signalled, but `copy validation fence not signalled within 250 ms` and `texture staging fence not signalled`: **no readback pixels were ever printed**. Death at ~10.3 s right after `texture upload 4 of 240x180 format 7` while reading `globals_textures.bnk` (292 MB offset); crash block unchanged (`98A1A23D...`). Remaining skips: 540 `no_render_targets`, 3 `no_rasterization`. |
| 064 | E569CBB6 | **first pixel proof: zeros.** `[upload-validate] MISMATCH via own fence: 8 samples differ (first at byte 0): src=5A rb=00; checksum 08D4A082; first pixels [00000000 ...]` - the whole 64x64 destination reads zero after staging->A->B->readback. Fences exonerated (copy/staging fences miss only the first poll, then signal) and memory exonerated (2277 MiB free after GPU setup; heap 1240->1058 MiB through the uploads). Audio healthy. 181 presents, death still ~9.3 s on a `gui_textures.bnk` read, crash block unchanged. |
| 065 | E6C0F8E2 | **no copy command executes.** Diagnostic: `a buffer->buffer` fence TIMEOUT 250 ms, post=`AAAAAAAA AAAAAAAA` (still the pre-fill); `b buffer->image->buf` TIMEOUT, post=pre-fill; `c clear B to 0xC3` TIMEOUT, not even the clear ran; `d` inconclusive; `summary: a=FAIL b=FAIL c=FAIL`. So the standalone submit path never reaches the GPU - not a format/units/flags/2D-engine problem. Present fence still signs. Fix: uploads ride the frame list. **(Retracted 2026-11-18: the entries were never KICKED - copy-diag blocked the presenting thread inside its waits, present #179->#180 gap = the four 250 ms waits; see the fence-verdict section. The copies executed at #180's kick, after the readbacks.)** |
| 066 | BB6A8D87 | **uploads execute; death 250 ms later.** The standalone submit was dead: with uploads recorded into the frame list they run (`[upload] #1 960x720 ... recorded into the FRAME list, rowLength 4096 bytes, imageHeight 720 rows`, then #2 480x360, #3/#4 240x180). The carrying frame stalled (present #182 15:33:31.118 -> #183 15:33:31.488, 370 ms vs a ~35 ms cadence) and the process ended at 31.543. The 64x64 readback probe was armed but pre-empted. `[submit]` sites now print queue/device/fence/cmdbuf addresses. |
| 067 | D12DE792 | **copy fence times out, queue is fine - verdict retracted.** Copy-only bisect: tiled destination `flags 0x800 size 2949120 align 512`, derived pitch 4096 B = the rowLength passed, `copy fence TIMEOUT after up to 1000 ms`; pitch-linear destination rejected by deko3d (`size 0`). Then `present-fence SIGNALLED ... at present #1` and 180 presents followed, so the queue executes and the fence mechanism works - which points at the copy submission never reaching the GPU, not at a wedge. **(Mechanism named 2026-11-18: no kickoff until present #1 - the wait ended 52.255, present #1 kicked 52.313; the copy then rode that kick and was harmless. Run-068's waitIdle flushes/kicks and decides.)** |
| 068 | 537FBBEB | **BREAKTHROUGH: the copy EXECUTES and lands the pattern.** `[copy-sync] A rows: dkQueueWaitIdle RETURNED after 5 ms`, `fence state after WaitIdle = signalled (queue error state: no)`, readback `[5A5A5A5A 5A5A5A5A]`, `verdict EXECUTED`. Descriptor A canonical (rowLength 4096 B, imageHeight 720 rows, flags 0x800 tiled). The 2D engine, descriptor and fence are fine - the submission needed kicking/waiting. "Copy is the killer" DEAD; 057/058/065/067 timeouts were polling/submit artefacts. In-frame probe armed but death 240 ms later pre-empted its readback (fixed in 069: presenter-polled). 180 presents, same ~9.5 s death on a gui_textures.bnk read, crash block unchanged `98A1A23D` - F2 separate, not chased ahead of F1. |
| 069 | 72D72676 | **F1 measurable; death suspect named.** copy-sync EXECUTED again. `[guest-readback] #30/#150 1280x2048: fnv1a=73F2DDC5 IDENTICAL, first/grid FFFFFFFF, zero=6799360/10485760` - the presented surface is STATIC white: the presenter and copy path are NOT the fault; the guest never drew content into it. Per-frame census: `drew 1, skipped 24`, skip-census `empty=4096 masked=0` - all skips legitimate null draws; one real draw/frame = boot/static-screen state, not a menu. Death ~100-200 ms after the first upload batch is KICKED (uploads #1-4 recorded 37.15-.34, present #156 kicked 37.352, last line 37.449). **Format finding:** upload #1 is xenos 23 = k_24_8_FLOAT -> deko 45 **ZF32_X24S8 (depth)** - the RGBA8-only copy proofs never covered it; #2 deko 28 RGBA8, #3/#4 deko 52 RGB10A2. Depth formats are not a supported 2D-copy destination on Tegra (host-side finding; the API does not reject them - run-070 measures whether skipping them stops the death). Staging retirement did NOT run between upload #4 and death (it only runs on later uploads; there were none) - the age mechanism did not fire this run, but it is a latent bug (could free before a long frame submits) and is replaced by fence-witnessed cohorts in 070. EDRAM transfer (1/run) and depth resolve (1/run) are NOT per-frame blockers - stop treating them as the white-screen cause. |

### Run-048 result: break located inside sub_8233EEE0

`sub_8233EEE0` (the component update) calls the pass `sub_825F3050` at guest
`0x8233F064` with `r3 = [component+4]`, `r4=[component+16]`, `r5/r6` from its
arguments, `r6 = component+0x3C`, `r7 = argument 6`. That explains the single
`[gui-pass] self=404BC8A0`: it is the component's `+4` manager, not the
component.

Before that call the function contains a state check on `[r3+36]`, a loop with
`DelayZero_82CBC6B0(100)` and a global byte at `[r29-6572]`. The 16:10:02 update
did **not** reach `0x8233F064` (no second `[gui-pass]`), so it took the other
branch of that state/wait logic. The break is therefore between
`0x8233EFD4` and `0x8233F064`, not in the writer and not in the state machine.

Timing caveat: `[gui-upd]` is logged **after** `sub_8233EEE0` returns, so its
16:10:02.527 timestamp does not prove the update ran after the 16:09:58.425
`[gui-pass]`; the update call may have started at 16:09:58.42, reached the pass
at .425 and returned 4.1 s later. Run-049 places markers **inside** the
function (before the pass call and at the wait-loop head) to settle the order
and the branch directly.

Next probe (run-049, package ready): mid-asm markers at `0x8233F010` (pass
path) and `0x8233EFD4` (wait loop), logging the component, `[component+36]`,
the manager, `n24`/`h2`, the loop byte and the live `r3`. Then fix in run-050.

### Run-049 pre-read - STATIC ONLY, no hardware run yet (unverified)

Read from the generated code (`port/generated/default/fable_2_recomp.119.cpp`
for `sub_8233EEE0`, `.222.cpp` for `sub_825F3050`, `.134.cpp` for
`sub_825F45A8`) against the 048 log. This **contradicts the "never reaches the
pass" premise** and predates run-049's markers; treat as a hypothesis to be
confirmed or killed by the 049 log, not as a result.

- `sub_825F3050` has **exactly one call site**, `0x8233F060` inside
  `sub_8233EEE0`; `sub_8234EC08` has **exactly one**, `0x825F3538` inside
  `sub_825F3050`. The `[gui-pass]` hook counter (cap 20) logged once, the
  `[gui-upd]` hook (cap 40) logged once -> **one update call total**, and the
  58.425 `[gui-pass]` belongs to it. The 4.1 s between `[gui-pass]` and the
  02.527 `[gui-upd]` is therefore spent **inside** the pass, not before it.
- The pass calls `sub_825F45A8(...)` at `0x825F3208` and jumps to the cleanup
  path `loc_825F389C` when it returns 0 (`if (eq) goto loc_825F389C`), skipping
  the whole batch body including the writer.
- The writer gate (`0x825F3500..0x825F3540`) is:
  `hash = sub_8233F2D8([r26+4],r4,r5)`, `wrapper = [[r26+4]+0x24]`,
  `if (wrapper && [wrapper+4].180 == hash) r25 = wrapper; else r25 = r31`.
  `r31` is `0` for the entire function (set at `0x825F3104`, never rewritten),
  so the else branch dereferences guest 0x4 - a **different fault** that no run
  has ever produced. Reaching that gate without the hash matching is therefore
  impossible on this evidence, and a matching hash would have logged
  `[gui-writer] #1` (single call site, cap 80).
- Only consistent path: the pass **was** entered, ran 4.1 s, `sub_825F45A8`
  returned 0, and the pass exited at `loc_825F389C` without touching the
  control. The null `+0x64` is downstream of an **empty/not-ready UI batch**,
  not of the `[0x834BE654]` wait loop or the state machine.

Prediction for run-049: `[gui-mark] pass self=42220210` (and `[gui-pass]` right
after it), **no** repeating `[gui-mark] wait`. If that holds, the fix target is
the input of `sub_825F45A8`, and a fix test needs one more in-pass datapoint
(its return + which batch list is empty) unless the 049 log already gives it.

### Run-049 result: CONFIRMED - the pass runs, the batch is the gate

Prediction held exactly. From `docs/runs/049/fable_2_049.log` (tag
`m1-witness-049`, build `F2EB4C2E`, crash block `270D8442...`):

```
22.413  [gui-mark] pass #1 self=42220210 w36=40537DC0 mgr=404BC8A0
        n24=40537DC0 h2=42538C10 loop=00000000 r3=40558D98 w3_36=40558DC8
22.417  [gui-pass] #1 self=404BC8A0                 (the pass call, 4 ms later)
26.527  [gui-upd] #1 ... h2=42538C10 h2+64=00000000  (pass returned 4.1 s later)
26.656  [gui-witness] h=42538C10 w64=00000000        (draw faults 130 ms later)
```

- `[gui-mark] wait` never fired: the `[0x834BE654]` loop is not involved, and
  the state machine is not skipping the update.
- The update reaches the pass carrying `h2=42538C10` (the faulting
  `CECPlayerControl`) and the pass spends the 4.1 s inside itself.
- Combined with the single call site of `sub_825F3050` and the single call site
  of `sub_8234EC08`, the pass cannot have reached the writer gate (that would
  log `[gui-writer] #1` or fault on guest 0x4). It exits at `loc_825F389C`
  because `sub_825F45A8` returned 0: **the UI batch it builds is empty or not
  ready**, so the whole update body - including the `+0x64` writer - is skipped.

Run-050 (`docs/RUN_050.md`, build `837831EF`, tag `m1-probe-050`) instruments
that batch: return value, the twelve vector counts, the one bit test that can
return 0, and the writer gate as a control. Run-051 is the fix test.

### Run-050 result: the batch lookup misses; producer still open

From `docs/runs/050/fable_2_050.log` (tag `m1-probe-050`, crash block
`32D5C1D1...`):

```
26.489  [gui-mark] pass #1 self=42220210 ... h2=42538C10 loop=0
26.492  [gui-pass] #1 self=404AF710
26.494  [NtCreateFile] FAILED: path='game:\data\' -> 0xc00000ba
30.518  [gui-batch] bit #1 ret=0 k0=40102D60 '...Data\Worlds\...'
30.520  [gui-batch] fn #1 -> 0 ; br #1 ret=0 c=[0 x12] ; exit #1 n12=0
30.546  [gui-upd] #1 ... h2+64=0        (pass returned after 4.05 s)
30.674  witness -> crash 0.16 s later
```

- The gate probe never fired, so the pass cannot have reached the writer.
- `CheckAndExtractBit_82C63040` is a two-line test: `[obj+8] == 0 -> 0`, else a
  locked read of the subobject and "is it non-null". It is a **name -> object
  lookup**, not a flag read. The lookup misses.
- The key template is exactly `Data\Worlds\` (12 bytes, NUL-padded at vaddr
  `0x820A7810`, one of 15 roots: `Data\TestWorlds\`, `Art\GUI\`,
  `Data\Art\GUI\Gameface\`, ...). The per-run key is composed from it by
  `sub_825F4310`; run 050's dump started at the string object, so only the
  template is visible. **The composed key is still unnamed.**
- Case sensitivity is ruled out as the differentiator: `game_files\data\worlds`
  is lowercase and `data\dir.manifest` lists `worlds\...`, but NTFS (oracle)
  and exFAT/FAT32 (Switch SD) both resolve case-insensitively, and neither our
  log nor the PC oracle logs contain any `worlds` file I/O at this point.
- The 4.05 s inside `sub_825F45A8` happens **before** the bit test, with no I/O
  and no guest-thread log line; the GPU queue had been wedged for 25 s (upload
  stalled at 19:31:01, present timeouts every ~2 s), so the app runs below
  1 fps at that point.

Run-051 (`docs/RUN_051.md`, build `90996B1B`, tag `m1-probe-051`) logs the
composed key and the key builder's duration. No fix yet: patching without the
producer named would be speculative.

### Run-051 result: key named, producer still open; run-052 is the fix test

```
[gui-key] enter #1 tmpl=8349750C ptr=40102D60 (template 'Data\Worlds\')
[gui-key] exit  #1 chars '...ds\Albion\...'
19:46:08.877 -> 19:46:08.879 = 2 ms
[gui-batch] bit ret=0, c=[0 x12], exit, gate silent, crash 150 ms later
```

- **Key:** `Data\Worlds\Albion\`. The template `Data\Worlds\` is at
  `0x820A7810`; the literal `Albion` sits at `0x820A6268` next to
  `LevelData`, `layers`, `LevelNameTextTags`, `level`, `.bnk`, `.adb` - a
  world/bank naming table, not a file path. There is no `Data\Worlds\Albion`
  literal in the image: it is composed at runtime.
- **The key builder is fast (2 ms)**, so the 4.0 s between `[gui-pass]` and the
  batch test is in the four calls `sub_825F3050` makes before it:
  `sub_825F2E10`, `sub_82369EE0`, `sub_8236A460`, `sub_82A492B0`. It is **not**
  I/O: the 15 s io summary covering the window shows 2 opens / 0.6 MB.
- **Producer not named.** Ruled out: case sensitivity (both hosts resolve
  case-insensitively; the key is a registry name, not a file open), missing
  content (`game_files` and the PC install have the same `worlds\albion`
  layout), the key builder, and file I/O. What remains is the world/bank mount
  that should register `Data\Worlds\Albion\`, and the 4 s wait is the strongest
  lead for why it has not by 40 s. This is tracked, and it no longer blocks the
  crash fix.

### Run-052 result: guard too narrow - the null is consumed by the element rect path

`[gui-guard] #1 skipped sub_822358A8 with r3=0 (lr=82390590)` at 20:30:53.964,
then 3 ms later the same `read of guest 0x00000024` from another site (crash
block `E5085770...`). The record's `pc` is stale again; decoding by instruction
and frame chain:

- `0xb8624824` is a guest `lwz rX, 0(r4)` with **r4 = 0x24**, i.e. a null
  pointer plus 36, not `lwz rX, 0x24(r3)`;
- frame `#00 sub_82426ED8+0x170` is the call to `sub_822659E8`, which does
  `addi r4, r31, 36` and calls `sub_82265A80`.

`sub_82426ED8` is the element rect/blit routine. On entry it builds
`r21 = [[[[r3+4]+4]+36]+4]+0x64` - the same page set - and walks it
(`[r21+8]`, `[r21+24]`, ...) to compose each tile's source rect. So a null page
set invalidates the whole element, and a per-callee guard can only move the
fault.

`[gui-phase]` timings: `825F2E10` 49.691, `82369EE0` 49.693, `8236A460`
49.699, `82A492B0` 51.752 - that call alone is **~2.05 s**, and roughly 2.2 s
sits in the gap after it, before the batch test. Both remain the producer lead.

### Run-053 result: third consumer -> choke-point guard (run-054)

Both guards fired (`[gui-guard] #1` on `sub_822358A8`, `[gui-guard] element #1`
on `sub_82426ED8`) and the fault still came 3 ms later, same instruction
(`0xb8624824`), same `far = 0x24` (crash block `C404B2DF...`). The site is
`sub_82265A80` reached from **`sub_822659E8`** - a clip/rect helper with ~75
callers. It does `addi r4, r5, 36`, so **r5 == 0 always dereferences guest
0x24**, whoever the caller is. Guarding callers cannot converge; the choke point
is the helper. Run-054 intercepts it: r5 == 0 -> zero the caller's output rect
(an empty rect draws nothing) and skip. Both earlier guards stay.

### Run-054 result: page-set class closed; network null class opened

Every guard held - `[gui-guard] #1/#2` on `sub_822358A8`, `element #1`, `clip
#1/#2/#3` - and `read of guest 0x00000024` never recurred. Survivability rose
~45 s -> ~50 s. The crash then moved to a **new** class (block `A11477CC...`):

```
21:43:12.9  [svr2011] net: socket(2, 1, 0) failed: 10022
            Unhandled guest access violation: read of guest 0x00000000
            pc/lr  sub_821C95D8+0xD8   far=0   x0=0
```

`NetDll_socket_entry` (`xam_net.cpp:855`) builds an `XSocket`; `Initialize`
(`xsocket.cpp:59`) calls plain `socket(af,type,proto)`. **Nothing in the port
ever calls `socketInitializeDefault()`**, which Horizon requires, so `socket()`
returns `EINVAL` -> the HLE reports `WSAEINVAL` (10022) and returns -1, and the
guest's online init dereferences the socket it never received.

### Run-055 result: the socket was a red herring

`socketInitializeDefault` works (`[socket] Horizon BSD service initialised`) and
`socket(2, 1, 0) failed: 10022` no longer appears - but the crash is **the same
instruction as run 054**: `sub_821C95D8 + 0xD8`, `0xb8606a80`, `far = 0`,
`x0 = 0`, same `x2 = 0x4F7C3A78`, now at ~52 s instead of ~50 s. So the socket
failure only shifted timing; it did not form the null.

`sub_821C95D8` is a **script/VM dispatch wrapper**: it calls `sub_822281F8`
(fetch argument) with indices 1 and 2, reads the returned object's `+0` and
`+4`, makes an indirect call through that pair, then writes the result through
`[r31+8]`. The null read is one of those argument-object dereferences - a script
value that is absent, not a socket.

Open: which script/VM call passes the missing argument, and why only after ~52 s
(menu idle -> some script tick). Needs the caller capture, not a guard.

### Run-056 scope (two flagged variables)

1. Crash: capture the VM call context at `sub_821C95D8` (arguments + caller LR)
   in one run, then fix the missing value at its producer.
2. Upload wedge (required): a 64x64 one-shot upload self-test with variants
   (no in-list barrier, `CpuUncached` staging, untiled destination) to isolate
   the `copyBufferToImage` hang, then the real fix so the display can live.

### Run-056 package (built, UNVERIFIED - `docs/RUN_056.md`)

Build `C2040A46BBFE0EAB72D5380023AF21AC6F777725E269AE4900432801B28063C5`, tag
`m1-probe-056` (staged in `stage/switch/Fable II`; `fable_2.toml` carries
`deko3d_upload_probe = true`). Wire check on the built ELF: the argument
wrapper's call at `0x3B908EC` is `bl 0x48F47C0 <AccessElement8_82A1E620>`, i.e.
the accessor hook really replaced the generated body.

Crash side (`port/src/core/vm_null_probe.cpp`, new file, added to
`FABLE2_SOURCES`). Two passive hooks, no guard, no skip - the run still ends at
the fault:

- `AccessElement8_82A1E620` (the VM argument accessor; strong `__imp__` body +
  weak alias, so a hook needs no codegen). It logs `[vm-fetch] live` once (proof
  the override linked), and for every call made by the argument-fetch wrapper
  (guest LR `0x82228208`) whose tag is **not 2 and not 7** it dumps `[vm-null]`:
  vm, argument index, element, tag, value word, the five slots around it, the
  VM's first twelve words, the value's characters (raw and at +16), the last
  eight fetches and the last eight dispatches.
- `sub_821C95D8` logs the first 24 dispatches (`[vm-call]`, caller LR + r4-r7)
  and keeps the last eight in a ring, so the crash line is read with its caller.

Upload side (`sdk/src/graphics/deko3d/texture_cache.cpp`, cvar
`deko3d_upload_probe` = true in the shipped `fable_2.toml`). One-shot 64x64
RGBA8 `copyBufferToImage` at texture-cache init, before the guest submits any
GPU work; each variant has its own image, staging block and fence, bound to 2 s:

| # | variant | single difference from the failing path |
|---|---------|------------------------------------------|
| 1 | `control-presenter-flags` | destination created like the presenter's images (`Render\|Present\|2DEngine\|HwCompression`) |
| 2 | `baseline-2dengine` | none - today's failing path at 64x64 |
| 3 | `staging-cpuuncached` | staging block CPU-**uncached** |
| 4 | `dest-pitchlinear` | destination pitch-linear (untiled) |
| 5 | `no-inlist-barrier` | the in-list barrier removed |

Order is deliberate: a hanging copy wedges the queue for the rest of the
session, so only the variants up to the first failure answer anything, and the
control has to run first. Reading:

- 1 TIMEOUT -> `copyBufferToImage` is unusable on this queue at any shape; the
  upload must move to a DKSH pass or to `copyImage` from a staging image.
- 1 PASS, 2 TIMEOUT -> the texture image creation is the difference (flags).
- 3/4/5 vs 2 -> that single variable is the culprit.
- all PASS -> the 64x64 shape is fine and the real upload's size/format/pitch is
  what differs.

Estimated cost: <= 10 s of boot on a wedged variant (2 s bound + teardown).

### Run-058 result: the fence primitive is dead; the upload fix holds

```
22:28:11.257 [upload-probe] fence-only:  TIMEOUT in 1000 ms (queue error state 0)
22:28:12.259 [upload-probe] clear-only:  TIMEOUT in 1000 ms
22:28:16.273 [upload-probe] summary: the FENCE is the problem: dkFenceWait never
             observed a signal for work that the queue demonstrably completed
22:28:23.608 deko3d: uploaded texture 2D (960x720, format 23) without a fence wait
22:28:23.930 deko3d: fence wait timed out for previous frame's render list after 250 ms
22:28:23.933 giving up on render recording; the last frame presented stays on screen
```

- `fence-only` signals a fence on an **empty** command list and still times out, so
  the primitive itself never reports completion on this stack. Every remaining wait
  is therefore pure cost.
- The no-wait upload works: the first real 960x720 texture was submitted with no
  wait and no stall.
- The remaining blocker was the **resolve/render-list poll**: 250 ms after the
  upload it failed and set `rendering_stopped_`, i.e. `giving up on render
  recording`. That is the freeze, and it is not a GPU fault.

Run-059 (built, `docs/RUN_059.md`) removes every remaining wait in the display
path (`PollFence`: one zero-timeout poll, never blocks, never stops recording:
present copy, `EnsureRecording`, `WaitForPendingList`, `SubmitAndWaitForReuse`),
logs one `[present]` line per presented frame (`slot`, `branch`, `guest#`, `age`,
`since_refresh`), and turns the upload self-test off by default now that its
verdict is recorded.

### Host-side finding (run-058, no run needed): the missing argument is an ENTITY

Correction to an earlier byte-order read: the crash registers hold
`x12 = 0x69784574656B7261`, whose **low-to-high** bytes are `arketExi`, and
`x13 = 0x2E27657461475F74` -> `t_Gate'.`. The fragment is therefore
**`arketExit_Gate'.`**, not `arkerExit_Gate`. That text is a **quoted name**.

It is in `data\levels.bnk`: the level entity manifest contains

```
<Entity name="BowerstoneMarketExit_Gate">0xE6CCB784</Entity>
```

(and `CemetaryExit_Gate` = `0x0E3ADD3B`; five identical copies of the entity
table in that file). A scan of `data\scripts`, `data\scripts_r`,
`gamescripts*.bnk`, `guiscripts.bnk` and `default_image.bin` finds **no**
`Exit_Gate` text and **no** reference to the GUID in either byte order, so the
engine resolves this entity **by name at runtime** - which is exactly what was in
the registers when the binding dereferenced the nil.

Consequences:

- **What argument 1 should hold**: the entity `BowerstoneMarketExit_Gate`
  (`0xE6CCB784`) as a type-2 object reference (its `[+0]` is a handle that
  `sub_82227680` unwraps through `[+4] -> [+0]`).
- **What the nil means**: the by-name entity lookup for a **Bowerstone Market
  (Albion) world entity** returned nothing.
- **Unifying hypothesis: now strongly supported (still not proven).** The GUI
  side misses `Data\Worlds\Albion\`; this side misses an entity that lives in
  that same world. Both are "lookup into the Albion world comes back empty",
  which is what an **incomplete world registration/mount** would produce. It is
  no longer two unrelated lookups - but the entity table itself could still be
  present while a sub-registration is missing, so the fix stays targeted at the
  producer the run-059 witness names, not at a guess.
- **run-059 witness is now cheap and specific**: hook the by-name entity lookup
  and log `(name, result)` for every miss (or at least every `*Exit_Gate`), plus
  the size/first entries of the entity table. If `BowerstoneMarketExit_Gate`
  misses while the table is populated, the producer is the registration of that
  world's entities; if the table is empty, it is the mount that never ran.

### Run-056 result: the argument is nil, and the caller is the VM itself

From `docs/runs/056/fable_2_056.log` (tag `m1-probe-056`, tag confirmed line 4):

```
22:14:31.111 [vm-call] #1 tid=7 caller=82228AF4 vm=507AAB88 r4=4F7C3A70 r6=50A97E44
22:14:31.113 [vm-null] #1 vm=507AAB88 index=1 elem=4F7C3A78 type=0 value=0
            slot idx=2 elem=4F7C3A80 value=0 type=1 (bool false)
            slot idx=3 elem=4F7C3A88 value=1 type=1 (bool true)
            vm words: +8=4F7C3A90 +C=4F7C3A78 ...
```

- Argument slots live at `[vm+12] + (index-1)*8` as `{value,type}`; index 1 is
  the first argument and it is **nil**: `type = 0`, `value = 0`.
- **Type 0 is also how the VM fills an omitted parameter** (the invoke helper
  writes `type = 0` and advances `[vm+8]` at `0x82228918..0x82228934`), so a nil
  here means "the caller supplied nothing" - not a wrong-typed value.
- The binding was called twice in the same burst with a **valid** argument
  (`idx=1 elem=4F7C3AC8 type=7 value=50C0A064`) and then once with nil, so one
  entry of a call run is missing, not the whole run.
- **Caller**: `lr = 0x82228AF4` is the return address of a `bctrl` inside guest
  function `0x82228830` (manifest alias `GuildChest_ProcessItem_82228830` - the
  name is from the earlier chest work; the code is the **VM invoke helper**). It
  reads the callee's type at `[r4+4]`, takes a *string* callee (type 6) as a
  named invocation, otherwise resolves the value with `sub_82A2E400`, reads the
  prototype at `[callee+16]` (param counts at `+73/+74/+75`), fills the missing
  parameters with type 0, and dispatches through `[[vm+20]+4] -> [+0] ->
  vtable[+16]`. That dispatch is how `sub_821C95D8` is entered.
- Consequence: **the nil is not produced at the call site** - it was already in
  the script's argument frame. The producer is whichever earlier call returned
  nothing, which run-057 watches (invoke ring, callee and argument strings).
- Upload side: **zero `[upload-probe]` lines**. `deko3d_texture_upload = true`
  did take effect (uploads ran at 22:13:49), so the toml was read; the probe key
  was not in that file and the cvar defaulted to false, and nothing logged the
  gate, so the silence was indistinguishable from a failed read. Run-057 makes
  the probe default-on and logs the gate either way.

### Run-055 socket change (landed, verified by log)

`XSocket::Initialize` does a lazy `std::call_once(socketInitializeDefault)`
under `__SWITCH__` and retries `socket()` once, logging
`[socket] Horizon BSD service initialised` (or the failing `Result`). Other
platforms untouched.

### Presenter status (from run 054)

```
[present] branch=4 -> swap 0: presented (placeholder)
[present] branch=1 target=1280x2048 guest=1280x720 -> swap 1/2: presented (guest output)
21:42:41.860 giving up on render recording; the last frame presented stays on screen
21:42:43.867 present copy fence timed out ... presenting anyway
```

The guest output is attached and two real frames reached the swapchain before
the queue wedged (~16 s in). The white screen is the M0 wedge, not a wrong
source image; run-054's present-on-timeout stops the display freezing at frame
0.

### P1 white screen - mechanism

User confirmation: white since deko3d was introduced; the placeholder was never
replaced by live frames.

Root cause: the **first texture upload's `copyBufferToImage` never completes**.
`sdk/src/graphics/deko3d/texture_cache.cpp:406` submits it on the device queue
and waits 5 s; the timeout fires ("fence wait timed out for texture upload"),
uploads are disabled, and from then on the queue is dead - the next frame's
record fence times out too (`giving up on render recording` at ~17 s) and every
present copy after that times out. The presenter is fine: `branch=1` with
`target=1280x2048 guest=1280x720`, and swaps 1 and 2 really were presented as
guest output before the wedge.

There is no display path around the queue: the present copy (image copy,
buffer->image copy, or CPU upload + copy) is all GPU work on the same device
queue, so a live display is impossible until the copy completes. Already ruled
out by earlier sessions: block alignment, copy shape (BCn / 16-byte pitch),
4096-byte padded pitch, plain RGBA8. Remaining suspect: the destination image's
creation (flags/layout) - it uses `DkImageFlags_Usage2DEngine`, single mip,
`GpuCached | Image` memory.

Next step (run-056): a one-shot boot self-test that copies a tiny (64x64 RGBA8)
texture through the same path and logs whether the fence signals, with variants
(no barrier in the list, `CpuUncached` staging memory, linear-untiled
destination). That isolates the hang without risking a full run.

Run-054 adds `[present] branch=` (which source feeds the swapchain; branch 4 =
never attached) and presents the slot on copy-fence timeout instead of freezing.
Fix test v3 = that plus the clip guard.

### Run-045 result: the gate is the HUD state machine

`sub_8234EC08` (the `+0x64` writer) has one call site: `sub_825F3050`, reached
only from `sub_8233EEE0`, called by `sub_8245D088` (the HUD state machine) in
case 2 (`this+44 == 2`) and only when `byte [this+0x32] != 0`. Otherwise that
branch runs the player lookup `sub_8233F2D8` + `sub_82353428` and **skips the
update**. Run 045's zero writer lines prove the skip branch was taken (or case 2
was never entered) before the fault.

So `+8` and `+0x64` are null because the player-control update never ran for this
instance; the class identification was right, the writer was correctly hooked.
The remaining question is the gate itself: state vs flag. Run 046 logs both.

## M1 crash analysis - fault and chain ESTABLISHED; producer open

### The fault (was: "record internally inconsistent")

Run 038's record is **coherent except for its `pc` field**. The faulting
instruction is guest `0x822356D0` in `sub_82235698` (host vaddr `0x212f920`,
guest entry `0x82235698`; the same instruction is at host `0x212f9e4`). It is an
**indexed** guest load, not an immediate one:

```
212f9d4  ldr  w4, [x0]            ; guest r3 (0)
212f9dc  add  w4, w4, #0x24
212f9e4  ldr  w4, [x1, w4, uxtw]  ; guest 822356D0: lwz r9, 0x24(r3)
```

The word `0xb8644824` is a register-indexed `ldr` (`[x1, w4, uxtw]`), not the
`ldur w4,[x1,#0x24]` the earlier session read into it; the immediate form would
need the fault address as a constant, which is exactly why the record looked
inconsistent. With the indexed decode the record is consistent.

The guest side was re-checked directly against `port/default_image.bin` with a
PPC disassembler: `0x822356D0  81230024  lwz r9, 0x24(r3)`, and the function's
first instructions (`lis r11,-0x7cce; addi r11,-0x5dd8` = `0x8331A228`) match the
jump table the generated C++ uses. The whole mapping (image base `0x82000000`) is
confirmed.

- `x1` is the guest memory base (guest 0 maps to `0x30f51da000`, the value in the
  record), so `far = x1 + 0x24` and the SDK's "read of guest 0x00000024" is the
  same instruction.
- The record's `x21 = 0x82235878` is the guest LR of the **second**
  `sub_82235698` call inside `sub_822357D0`; `x22 = 0x822359b0` is the guest LR
  after the `sub_82235AC8` call in `sub_822358A8`. Both belong to this chain.
- The instruction word is not unique in the ELF (3709 occurrences across code
  and data), so the identification rests on the semantics, not on uniqueness:
  `w4 = guest r3 + 0x24` then load through the guest base, which is exactly the
  reported `far`. The recorded `pc` is the only field that disagrees.
- Only `pc` (imagen+0x212ef34) is wrong (it lands 0xAB0 before the real
  instruction, in the neighbouring function). Do not use that field; the
  instruction, `far` and LR values are the usable parts.

`AUTHORITATIVE BASE ARITHMETIC` still holds: `base = &_start` = ELF vaddr 0, so
`imagen+0x...` is the ELF vaddr. It is the *pc* value that is stale here, not the
arithmetic.

### The call chain

```
sub_822358A8(r3)          ; r3 = the null object
  -> sub_822357D0         ; prologue saves r3 (mr r30,r3), then mr r3,r30
     -> sub_82235698      ; second of four calls (LR 0x82235878)
        lwz r9, 0x24(r3)  ; guest 0x822356D0, fault when r3 = 0
```

`sub_822358A8` itself does not touch r3 until that call: it uses r4 (the GUI
element) at entry (`[r4+4] -> [+124] -> vtable+72`), and passes r3 only to
`sub_822357D0` (call at guest `0x822359A0`, LR `0x822359A4`).

### Where r3 comes from

All **six** call sites of `sub_822358A8` compute r3 the same way:

```
lis  r11, -31927              ; r11 = 0x83490000
lwz  r11, 26912(r11)          ; L0 = [0x83496920]
lwz  r10, 12(r11)             ; L1 = [L0+0xC]
lwz  r9,  88(r10)             ; L2 = [L1+0x58]
lwz  r8,  4(r9)               ; L3 = [L2+4]
lwz  r7,  8(r8)               ; L4 = [L3+8]
lwz  r6,  4(r7)               ; L5 = [L4+4]
lwz  r11, 36(r6)              ; L6 = [L5+0x24]   (explicitly null-checked)
beq  skip                     ; if L6 == 0, the caller skips
lwz  r11, 4(r11)              ; L7 = [L6+4]
lwz  r3,  100(r11)            ; r3 = [L7+0x64]
```

Call sites (guest return address -> enclosing function):

| LR | caller |
|----|--------|
| 0x822050C4 | sub_82204F40 |
| 0x82390590 | sub_8238FFA8 |
| 0x82457B7C | sub_82457870 |
| 0x82459908 | sub_824596B0 |
| 0x82749BEC | sub_82749910 |
| 0x82781734 | sub_827814D8 |

Because the fault fires at `+0x24` *inside the callee*, not at `+0x64` in the
caller, L0..L7 were all valid and **L7 was a live object whose field `+0x64` was
0**. That is the null: a guest object below the null-checked L6 exists, but the
pointer at `+0x64` was never populated. This is a guest data gap, not a bad
address translation.

### Provenance (run 043, witness fired)

At 06:06:47.794, 43 s in (uploads enabled), the witness fired with:

```
sub_822358A8 r3=0 lr=82390590 r4=425DDB90 r5=00000000 r6=42220210 r7=404B9C50
83496920 a=40102AA0 b=4252B730 c=404B9C58 d=4216D6B0
         e=404B9C50 f=42220210 g=40530D88 h=42538C10 i=00000000
```

- **Caller** `lr=0x82390590` = the `sub_8238FFA8` site in the table above.
- **Object** `h=0x42538C10` is live; `i=[h+0x64]=0`. The class is not yet named.
- `r4=0x425DDB90` is the GUI element being drawn; `r6=0x42220210` is L5.
- The violation follows 9 ms later: `read of guest 0x00000024`, same fault.

The chain matches the six call sites exactly, so this is the known fault and the
object is the one `sub_82235698` treats as a page (it reads `+8` table, `+36/+40`
origin and the pointed-to `+0x64`), which is the GUI texture/page set used to
build the element's UV rects.

### Class and writer (run 044)

`[h]` is not a vtable: it is the class type descriptor at `0x820A687C`
`{deleting destructor 0x8234E398, "CECPlayerControl"}`. The instance is a
**CECPlayerControl** component:

- constructor `sub_8234E2B0` (stores the descriptor at `+0`, zeroes `+8` and
  `+0x64` among many fields);
- destructor `sub_8234E3E8` (frees vectors at `+0x18`/`+0x74`, not `+8`/`+0x64`);
- **writer of `+0x64`: `sub_8234EC08`**, called from the update loop
  `sub_825F3050` at guest `0x825F3538`. It releases the old object at
  `this+0x64`, allocates 56 bytes, builds the object from the rect at
  `this+0x98..0xA4`, and stores it at `this+0x64`.

`+8` is not written by that method; it is part of the same component state and
also null, consistent with an instance whose update has not run on this path.

### Why the field is null on Switch

The constructor zeroes `+8`/`+0x64`; the update loop is what fills `+0x64`. The
menu HUD draws the component (`sub_8238FFA8` -> `sub_822358A8`) 1 ms after the
content enumeration, and the field is still 0. Two explanations remain, and run
045 separates them by hooking the writer:

1. **Ordering**: the update loop never reaches this instance before the draw
   (fresh component, or the loop's condition at `0x825F3510` picks another
   object). The PC's update/draw order differs.
2. **Allocation failure**: the update runs and stores 0 because the 56-byte
   allocation or its constructor returned null (the method releases the old
   pointer first, so a failure leaves 0).

No speculative patch: the result of run 045 picks the fix.



## Windows oracle - result

The PC release (`Fable 2 1.5.0\fable_2.exe`, same recompiled guest code, D3D12)
ran **170 s twice** (logs 006, 008) from boot through the menu window in which
the Switch crashes, with no `read of guest 0x00000024`; an earlier PC session
(log 005) went on into gameplay. Same guest code, no crash: the difference is the
host layer (runtime/ordering), not guest logic.

- `FABLE2_FUNC_TRACE*` strings exist in that binary, but no
  `fable2_func_trace.log` / `fable2_func_summary.log` is produced with the env
  vars set: that release does not include the `REX_FUNC_PROLOGUE` override, so
  the trace path cannot be used on this artifact.
- Tried to reproduce the Switch guest mode on the PC (720x480, the value in
  `fable_2.toml`) via the toml and via `--video_mode_width/height`: the swap
  source stayed 1280x720 and no violation appeared. **Inconclusive** - the cvar
  did not take effect there, so the video-mode hypothesis is neither confirmed
  nor refuted. Switch reports 720x480 to the guest; PC reports 1280x720.

## run-039 - prepared, unverified

Diagnostic build **0B0D5CBF** adds one passive witness at `sub_822358A8` entry
(`port/src/core/gui_null_witness.cpp`). It writes nothing to guest memory, runs
the real function unchanged, and when `r3 == 0` logs once - at warn level, which
the SDK flushes to the card immediately:

```
[gui-witness] #1 sub_822358A8 r3=0 lr=<caller> r4=.. r5=.. r6=.. r7=..
[gui-witness]   83496920 a=.. b=.. c=.. d=.. e=.. f=.. g=.. h=.. i=..
```

(`a..i` are L0's value and L1..L8 in the table above; `i` is r3.) It logs at
most four times, then stays silent. This turns the open question into one line:
`lr` names the caller, and the chain shows whether the break is at a link or
exactly at `+0x64`.

This run is **diagnostic only** - no setting changes. Instructions and what to
return: `docs/RUN_039.md`. Remove the witness source and rebuild once M1 closes.

### Run 039-042 + full history: a below-guest death gates the fault

The user's observation for run 041 settles the termination: **the app died by
itself at the ~9 s log terminus**, and it left no `rex_crash.log` block, no
guest-violation message and no witness line. The SDK reports host memory faults,
and `exit()`/`abort()`, through its crash writer; none of those fired. The death
is therefore **below the guest layer** - an OS kill, a GPU channel fault, or a
fatal path that never reaches the SDK reporter. It is **not** the known
`0x24` null-deref, and the witness had nothing to report.

The fault is real but is **not reachable on the current deko3d path**, because
the process dies at 8-9 s in the GUI texture load, before the menu frame that
eventually null-dereferences.

Evidence from the full log history (`docs/runs/history/`):

- The `read of guest 0x00000024` appears in runs **004, 005, 007, 028, 036,
  037** - and not in 038-041. In 036/037 it fires 0.2 s after a
  `[deko3d] present copy fence timed out`, i.e. only after the GPU path got
  that far.
- Configurations that survived past 9 s: the Vulkan runs (004/005/007 and the
  001-027 era) and the deko3d runs with **texture uploads enabled** (028, 036,
  037). Every run with deko3d and uploads disabled (038-041) dies at 8-9 s.
- 036/037 show a 3.9 s pause at ~10 s that ends with `deko3d: fence wait timed
  out for texture upload after 5000 ms` + `texture upload stalled`, and then
  they continue for minutes. In 038-041 that path never runs; the app reaches
  `first draw` and dies **0.1 s later** (041: first draw 05:24:35.894, last log
  line 05:24:36.005).
- Scene at the fault (all six runs): immediately before the violation the game
  lists marketplace content (`[guardado] listar tipo 00000002 ... 0 entrada(s)`)
  in both save roots, then faults. Run 041 performs the same listings at
  05:24:29.104 and the XMP empty-enumerator setup at 05:24:35.736, and dies
  0.3 s later. The enumerator itself is non-blocking (`xam_enum.cpp`), so this
  identifies the scene - the menu's content-list draw - not the death mechanism.
- Run 042 (all draws skipped) **still died at the same terminus**, with frames
  advancing (frame 127, `drew 0`) and no `[gpu-wait]` line. So the draw path is
  exonerated, and the death is abrupt rather than a wait over 2 s.
- The user's screen message for 042 - **"The software was closed because an
  error occurred"** - is the Horizon OS error screen. The death is a **fatal
  host fault/OS kill**, not a clean exit, and it reaches the OS before or
  without the SDK reporter.
- Present timeline in 042: the last swap is **swap 2 at 05:52:06.167**; frames
  continue to 127 at 05:52:11.001 and draws to 05:52:12.946, with **no further
  present** for the last ~6.8 s. The white picture the user saw was the last
  presented frame held on the display, not continuous presents. The fatal
  operation is therefore in the non-present phase: render target creation,
  clears, copies/resolves, texture-bank IO - not in a swap.
- **Free decisive datum, not yet returned:** AtmosphÃ¨re writes its own crash
  reports to `sd:/atmosphere/crash_reports/` (and `erpt_reports/` when present)
  for exactly this OS-level death. The newest file should name the faulting
  thread, exception type and PC. Requested; it can identify the fault without
  another hardware run.
- Two mechanisms remain:
  1. **GPU fault / OS kill in a non-draw operation.** No report is written for a
     killed process (035 and 041 are the same class). With draws skipped, the
     remaining GPU work is clears, copies/resolves and presents.
  2. **A timing/state difference in the texture-upload path.** This is the only
     discriminator in the whole history: uploads enabled (028/036/037) survived
     past 9 s; uploads disabled (038-042) die. Run 043 tests it directly.
- The last *completed* wait, `mem 0x1FC83006 ref 0x0 info 0x13`, is routine
  (hundreds per 5 s in 036/037, a few ms). With no `[gpu-wait]` line in 042, an
  unmatched PM4 wait is no longer the leading candidate.
- Build identity is proven: 041 logged `[fable2] build tag: m1-witness-041`.
- The witness is on the only path: `sub_822357D0` has exactly one caller
  (`sub_822358A8`) and `sub_82235698` exactly four, all in `sub_822357D0`;
  callers branch to the hook. When the fault is reachable again it will fire.

So the order of gates is: **a below-guest death at ~9 s â†’ no menu frames â†’ no
fault**. Whether that death is the unmatched wait or a GPU/OS kill is the open
question; M1 cannot be closed until the process survives this point.

### Run-043 result and run-044 package

Run 043 confirmed the uploads discriminator and delivered the M1 provenance:
uploads enabled, the 5 s upload fence timed out, the app survived to 43 s (past
character selection), the witness fired at the null field and the guest violation
followed. The no-upload death at 8-9 s (038-042) is a **separate defect**, tracked
but not chased until M1 is fixed.

Run 044 (`F328F67BD2E8DF76E0C3354BC107ED774000198E8CCE35A3D1617C0C879BA372`,
tag `m1-witness-044`) keeps uploads on and extends the witness to dump, at the
same point, `[h]`'s vtable and the page fields plus `r4`'s vtable and first words.
It writes nothing; the fault still happens. The vtable names `h`'s class, and
then the class's constructor/initializer (and therefore the `+0x64` writer) is
findable in the generated code.

Procedure: `docs/RUN_044.md`.


## Next actions

1. **Run-068 (hardware, built and staged - `docs/RUN_068.md`, tag `m1-fix-068`,
   NRO `7370D15571AF9DA9C4FFF2CC6BC1DE32C71A7F88C27EA975271DFDD8C8736876`):**
   one question only - does that copy execute. At setup, three `DkCopyBuf`
   descriptors (A rows 4096/720, B bytes 4096/2949120, C tight 0/0 with an
   unpadded 3840 pitch) are each submitted alone with the list/cmdbuf/source
   logged, the fence state before and after a **thread-bounded
   `dkQueueWaitIdle`**, and a 64x64 readback. Verdict per attempt:
   **EXECUTED / DROPPED / WEDGED**. Guest path, present blit and UI untouched; the
   older in-frame probes are off.
2. **Run-069 (from 068):** EXECUTED -> the copy is legal and the ~250 ms death is
   the draw that samples the uploaded image -> bisect the descriptor/sampler with a
   64x64 render target. DROPPED -> the command never reaches the GPU even though
   WaitIdle returns -> chase submission/command memory (run 066's `[submit]`
   addresses). WEDGED -> copy legality (flags, tiling, tile-aligned rect).
3. From the `[gui-phase]` timings (`82A492B0` ~2.05 s + ~2.2 s gap), name the
   wait in the pass setup and fix the `Data\Worlds\Albion\` registration at its
   producer, which also restores the skipped elements.
4. **PC launch pause**: no emulator/reference-runner/window launches on this PC
   until the hub lifts it (hardware runs are the user's, on their console).
5. Track, but do not chase now: the separate deko3d-with-uploads-disabled death
   at 8-9 s (038-042), a below-guest OS kill confirmed by the Horizon error.
6. After M1: rectangle lists (re-dump with
   `vulkan_force_expand_rectangle_lists_in_vs = true`, `convert_shaders.py`,
   `pack_shaders.py`; fill `vs_AB1E86137A0240E8`, `vs_D5E12A97B3A84881`,
   `ps_93E7F7B5D05626AA`; add the guarded `host_vertex_shader_type` fallback),
   then the texture upload mechanism (M3).

## Licence (user, 2026-11-17)

The port ships under **GPL-3.0**. `LICENSE` at the repo root now carries the full
GPL-3.0 text, and `CREDITS.md` names the reference projects
(NaGaa95/UnleashedRecomp-NX @ `84932ed0`, hedge-dev/UnleashedRecomp). Rules:

- copying a file from `H:\Projects\Fable2Port\_refs\UnleashedRecomp-NX` is
  allowed now; keep its copyright header, add a line naming the origin file, and
  list it in `CREDITS.md` in the same commit;
- only take what is used - no bulk copying;
- release notes must state GPL-3.0 and that the port includes code from
  UnleashedRecomp-NX; the public repo is the corresponding-source offer.

**Status: no file has been copied yet.** `os/switch_atomic_wait.h` was compared
and is not needed (see the sync audit in run-061): the submit epoch is a
non-blocking counter, the staging hand-off is release/acquire ordered, and
swap-image ownership belongs to deko3d. If a *blocking* wait is added later (audio
pump, frame pacing), Horizon's address arbiter with a 1 ms safety timeout - what
that header wraps - is the pattern to copy rather than a spin, because on Horizon
a yield never runs a lower-priority thread queued on the same core.
`hid/driver/switch_hid.cpp` stays parked for M2 input.

## Vulkan / NVK route - PARKED, not a fallback (verified 2026-11-17)

The reference fork does not make Vulkan available: it contains no Mesa/NVK
sources and no `.a`/`.so` binaries, its toolchain expects a `PLUME_PLATFORM_SWITCH`
that upstream renderbag/plume does not have, and its stubs reference a private
23-archive "Mesa-Switch NVK closure". Taking that route means building a Mesa NVK
backend, the Switch WSI and a NAK pipeline ourselves - a separate multi-week
project, not an escape hatch for deko3d. The renderer stays deko3d.

## Working rules

- **VISIBLE WINDOWS (user standing order, 2026-11-16): every Windows GUI run is a
  normal, visible window - no hidden desktop, no minimizing/hiding/killing the
  window, no suppression scripts or daemons. Quiet mode is lifted. Hardware
  packages are prepared but never run here.** See the section at the top.
- No game data, generated code, shader packs, NROs or DKSH in git.
- No commit or release without user approval.
- No claim without a hardware run; mark unverified changes as such.
- Three strikes on one approach: stop, document in this file, escalate.
- One measured variable per hardware run. A passive log hook does not count as a
  variable; a behavior change does.
