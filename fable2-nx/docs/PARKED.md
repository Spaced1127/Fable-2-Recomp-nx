# PARKED - changes, threads, confidence, and the one experiment that confirms each

Mode: BATCHED-CHANGE (no hardware available). Nothing here is claimed as a
device result. "Staged" = in the batch-079 package, compiled + static-asserted
+ host-tested + written argument (RUN_PLAN.md), device verification pending.

## Staged in batch-079 (awaiting the one run)

| # | change | confidence | single confirming experiment |
|---|---|---|---|
| 1 | **Prologue list** - uploads in a copy-only cmdbuf submitted+kicked before the draw list | **HIGH** - reproduces run-076's A=B=C=D=PASS configuration exactly (same list shape, same submit path, queue order puts it between completed render passes); the fault only ever occurred inside the draw list | run batch-079: **no `[gpu-error]` for 60 s** (1800+ presents). If it faults: correlation dump names inherited state; A/B `deko3d_upload_prologue=false` reproduces the old fault |
| 2 | **Prologue overflow cap** (128 copies -> mid-frame submit+reopen) | **HIGH** - same copy-only shape, static-asserted + host-tested to fit the 64 KB block; deko3d cmdbufs do not bounds-check | heavy-load frame shows multiple `[upload-prologue]` submits, no corruption/fault |
| 3 | **Toolchain static_asserts** (0x800/0x9/0x28/barrier 4/mask 0x15) | **CERTAIN** (compile-time) | build passes; a future toolchain drift fails the build loudly |
| 4 | **Late mirror re-read** at present >= 160 (passive) | **HIGH** (safe: CPU read of persistent shared memory) | `[upload-src-late] ... DATA ARRIVED LATE` vs `STILL EMPTY` - decides #5's direction |
| 5 | **Fence-witnessed ring reset** (EnsureRecording waits instead of polling before ResetPerFrameRings) | **HIGH** - restores ring_buffer.h's documented invariant; run-068 fence timing (~5 ms) + run-069 flush-at-submit make the wait cheap; removes a real CPU-overwrites-GPU-reads race at the frame boundary | no `[gpu-error]` AND no `ring reset` timeout lines for 60 s; A/B not needed (poll-only was provably unsafe) |
| 6 | **upload_math extraction + host test** | **CERTAIN** (behavior-identical refactor; host test ALL PASS incl. 1..2048 alignment sweep + run-076 staging sizes; static_asserts on host+target) | `upload_math_test` ALL PASS (done); on-device the 2x2 matrix is the regression guard |

## Static audits completed (run-079, no change needed - do not re-speculate)

- **Ring flush ordering**: constant/descriptor rings and converted indices are
  flushed PER DRAW before the draw command is recorded
  (command_processor.cpp:1561-1563), hence before submit - correct.
- **Ring reset**: was the one misuse (poll-only) - fixed, staged #5.
- **Present-copy command slots**: 16-slot rotation at ~30 Hz = ~0.5 s
  retention vs ~5 ms GPU execution; the in-order present fence guarantees
  completion long before reuse - safe by construction.
- **Empty/barrier-only lists**: SubmitAndWaitForReuse and Flush guard on
  has_commands_; cache invalidation is emitted lazily on first bind; the
  prologue opens only when a copy is recorded - the D2-era trap is covered.
- **cmdbuf lifecycle**: clear() -> addMemory() -> record -> finishList()
  ordering verified in prologue, EnsureRecording, presenter and probes.
- **Fence reuse**: this deko3d fork's fences are monotonic per-channel
  semaphores (Phase-1 disassembly); reuse without re-init is safe here.

## Parked threads (need a device to diagnose - NOT speculated in code)

| # | thread | confidence in cause | single confirming experiment |
|---|---|---|---|
| 5 | **Guest mirror empty at upload** (run-076: staging 0 non-zero CPU-side; textures land zeros) | UNKNOWN - candidates: data arrives after the upload (async loader; fix = re-upload/write-watching), wrong alias/base_page, or guest writes never reach the mirror | #4's late re-read: late -> implement re-upload on write-watch; never -> dump guest page permissions/aliases for base_page (device-only diagnosis) |
| 6 | **UV scale for padded images** (w64/h8 alignment pads 480->512; guest UVs would read the padding tail) | MEDIUM - real geometry mismatch, but only visible once textures have data | after #1+#5: menu art vs PC oracle screenshot; fix = logical/aligned UV scale in the DKSH path (M3) |
| 7 | **Depth textures via DKSH resolve** (ZF32_X24S8 skipped + never bound; shadows/depth-sampled effects need them) | LOW (unimplemented) | after menu visible: depth-sampled effects missing/wrong; implement a DKSH resolve pass |
| 8 | **VM-nil crash ~50 s** (`sub_821C95D8`, arg 1 nil, `BowerstoneMarketExit_Gate` by-name lookup; runs 054-058 evidence) | MEDIUM - named object + lookup site, producer unnamed | survive past 60 s (batch-078): `[vm-lookup]`/witness lines name table-empty vs entry-missing; fix at the producer |
| 9 | **RGB10A2 / other format sampling correctness** (deko 52 etc. via DKSH pack) | MEDIUM | after #5: per-format visual comparison vs PC; swizzle/format fixes in the pack |

## Retracted claims (do NOT re-derive - corrected in STATUS.md)

- "fences never signal on this stack" (057/058) -> missing-kickoff artefact;
  fences signal in ~5 ms once kicked (068).
- "the copy is the killer" / "standalone submit path dead" (065/067) -> same
  artefact; copies execute and land the pattern (068).
- "width%64 is the discriminator" (074) -> alignment did NOT fix the fault
  (075); kept as harmless layout hygiene only.
- "copies landed zeros" (074/075 probe verdicts) -> measurement artefact: the
  probe checked 0x5A against decoded GUEST data. The 2x2 with known content
  (076) proved copy/staging/image/flags all legal.
- "staging lifetime frees under the GPU" (069 hypothesis) -> no retirement ran
  between the last upload and death in that log; cohorts fixed the latent bug
  anyway, but it was not the observed death.
- "short reads at EOF reported as failures" (071 hypothesis) -> disproved on
  hardware (072): iosb.status=0, info=real count, PENDING+APC. Closed.
- "exit() is the death" (stale 057/058 crash block) -> runs 059-076 fired no
  exit hook; the death was the GPU channel fault (072 witness), now addressed
  by #1.

## Rules in force

- One measured variable per device run; three-strike rule per defect thread.
- No device result claimed without the log; logs archived unrenamed in
  docs/runs/0NN/ and diffed against the previous run.
- New fault needing a device -> stop the thread, note it here, no code
  speculation.
