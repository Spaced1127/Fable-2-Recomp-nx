# RUN_PLAN - the batch-078 package (one build, all queued fixes)

Build: `m1-batch-078`. NRO SHA-256:
`B41F1319A2440895CDC09676742FEE9F192AE5463B26690F80F3BEE2410FB649`.
Status: compiled clean + static-asserted + written argument below; **device
verification pending** (user unavailable - no hardware requests made).

## What the package carries

1. **Prologue list** (run-077): uploads recorded into a dedicated copy-only
   cmdbuf - no draws, no render targets, no texture binds - submitted and
   kicked at `IssueSwap` BEFORE the frame's draw list (`SubmitUploadPrologue`).
2. **Prologue overflow cap** (run-078): at 128 copies the accumulated list is
   submitted mid-frame and a fresh one opened (deko3d cmdbufs do not
   bounds-check; 128 x 512 B = 64 KB = the block, static-asserted).
3. **Toolchain static_asserts** (run-078): Usage2DEngine=0x800, staging
   flags=0x9, image mem-flags=0x28, DkBarrier_Full=4, invalidate mask=0x15 -
   the exact values the run-076 matrix logged; a toolchain drift stops the
   build instead of silently invalidating the evidence.
4. **Late mirror re-read** (run-078, passive): at present >= 160 the first 8
   uploads' guest source pages are re-read CPU-side and compared with their
   upload-time non-zero counts - the single experiment for the parked
   source-data thread (PARKED.md #2).
5. Kept from earlier runs: one-frame sampling delay, staging cohort pinning
   (fence-witnessed, never age), depth-format skip + never-bind, w64/h8 image
   alignment, copies-only probe (first 4 uploads), 2x2 matrix at setup,
   guest-output readback (#30/#150), skip census, death-watch + host-action
   ring, exit-path wraps + per-boot markers, GPU error witnesses (present /
   post-kick / frame-submit).

## Why each change is correct against run-076 (written argument)

- **Run-076 evidence:** A=B=C=D=PASS. Every cell was a COPY-ONLY list
  (copyBufferToImage + Full barrier, nothing bound), submitted via
  submitCommands + signalFence + flush, waited with waitIdle, read back with
  copyImageToBuffer. Sources: both staging kinds (flags 0x9). Destinations:
  setup image AND real texture-cache images (flags 0x800, mem 0x28, offset 0,
  aligned). All legal, all landed 0x5A.
- **Prologue = the isolated configuration, mid-stream.** The prologue list has
  exactly the cells' shape: copies + barriers only, nothing bound (a fresh
  deko3d list binds nothing; there is no render pass to end). It is submitted
  on the same queue via the same submit+flush path. Queue order guarantees it
  executes AFTER the previous frame's list completes and BEFORE this frame's
  draw list - i.e. between completed render passes, never inside one. The
  in-frame path (runs 066-076) recorded copies INTO the draw list with RTs
  bound and 3D state open - the only configuration that faulted. The one-frame
  sampling delay additionally guarantees no draw samples a freshly written
  image in the same submit (aliasing excluded).
- **Overflow cap:** a mid-frame prologue submit is the same copy-only shape;
  submitting it earlier only changes WHEN copies run, never their context.
  The cap is static-asserted to fit the command block.
- **Static asserts:** guard the constants the argument above relies on.
- **Late mirror re-read:** a CPU read of persistent shared memory - no GPU
  involvement, cannot fault the channel; it only ANSWERS the parked question
  (data late vs never), it does not speculate a fix in code.

## What to run when back (one package, in this order)

1. Copy the three files (no renames, ever):
   ```powershell
   $src = "H:\Projects\Fable2Port\stage\switch\Fable II"
   $dst = "E:\switch\Fable II"
   Copy-Item "$src\fable_2.nro","$src\fable_2.toml","$src\fable2_config.toml" $dst -Force
   (Get-FileHash "$dst\fable_2.nro" -Algorithm SHA256).Hash
   # must be B41F1319A2440895CDC09676742FEE9F192AE5463B26690F80F3BEE2410FB649
   ```
2. Launch full-memory. **Let it run 3+ minutes** (acceptance: 60 s survival).
3. Return the **whole `logs/` folder as-is, unrenamed, INCLUDING `logs/rex/`**
   (rex_crash.log, rex_actions.log, rex_exit_marker.log, rex_stderr.log) plus
   a photo of the screen and the wall-clock time.
4. If it survives 60 s: keep it running 5+ minutes and note when/if the menu
   appears; the ~50 s VM-nil witness (PARKED.md #5) may fire - that is expected
   progress, not a regression.

## Which question each log line answers

| log line | question |
|---|---|
| `[fable2] build tag: m1-batch-078` | identity - proves which package ran |
| `[copy-matrix] A/B/C/D ... PASS/FAIL` + verdict | are copy/staging/image still legal in isolation (regression guard) |
| `[upload-prologue] N copies ... before frame submit #K` | prologue riding ahead of the draw list (the fix is active) |
| `[upload] #N ... recorded into the PROLOGUE list` | every upload uses the copy-only path |
| `[gpu-error] ...` (any of the 3 witnesses) | **F2: does the channel still fault?** absent for 60 s = fix confirmed |
| `[death-watch] present #N ... gpu error state=no` + last actions | survival cadence + last stage if it dies |
| `[upload-src] guest mirror ... N non-zero of 4096` | mirror content AT upload (run-076: empty) |
| `[upload-src-late] ... now N (was M) -> DATA ARRIVED LATE / STILL EMPTY` | **parked #2: late vs never** - decides the next fix |
| `[upload-sync] probe #N ... DATA LANDED / ALL ZERO` | real-data copies land bytes (post-fix) |
| `[guest-readback] #30/#150 fnv1a=...` | **F1: does the guest output animate?** hashes differ = yes |
| `[skip-census] empty=... masked=...` | draws still legitimate nulls vs real binding faults |
| `rex_exit_marker.log` new `build=m1-batch-078` entry | a deliberate exit fired (which path) vs OS kill (absent) |
| `rex_crash.log` NEW block tagged m1-batch-078 | symbolise against port/build/fable_2.elf (identity map) |

## On-screen checklist (photo + notes)

- [ ] still pure white/static, or any new content (geometry, text, colour)?
- [ ] if content: does it look like Fable's boot/menu (positions, fonts)?
- [ ] any corruption patterns (stripes, tiles, garbage) - note where;
- [ ] does the screen CHANGE over time (animation) or stay frozen?
- [ ] HOME button responds? (process alive vs hung)
- [ ] any Horizon error dialog text - photograph it.

## A/B cvars (only if the primary result needs discriminating; one at a time)

| cvar | default | flip to | question |
|---|---|---|---|
| `deko3d_upload_prologue` | true | false | legacy in-draw-list path reproduces the fault (confirms prologue is the fix) |
| `deko3d_upload_sync_probe` | true | false | skip the first-4 synchronous probes |
| `deko3d_upload_probe_frame` | false | true | re-enable the in-frame 64x64 readback probe |
| `deko3d_guest_readback` | true | false | silence the #30/#150 readbacks |
| `deko3d_copy_matrix` | true | false | skip the setup 2x2 |
| `deko3d_upload_skip_depth` | true | false | re-allow depth copies (known-bad; A/B only) |
| `deko3d_upload_defer_sampling` | true | false | immediate binding (aliasing A/B) |

Set in `fable_2.toml` (flat keys), copy the toml with the NRO.
