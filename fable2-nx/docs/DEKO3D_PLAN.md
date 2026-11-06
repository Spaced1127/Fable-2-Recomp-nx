# Fable II - deko3d native renderer plan

## ROUTE CHANGE (2026-11-06): Renut is NOT applicable

The D3D-level route (Renut, as in sa2-nx/banjo-nx) was measured and rejected:

- A census build hooked **1,782 sub_ functions** in Fable's D3D candidate
  clusters (0x821F-0x822A draws/streams, 0x82B9-0x82BB device/display,
  0x82F6-0x82F7 shader area) and ran to the menu at 1.3-1.8 fps.
- Across the whole menu session the hooked D3D functions saw **8-13 calls per
  5 s** - only init/display helpers. The D3D draw functions are never called:
  `DrawIndexedVertices 0x8221E0F0`, `DrawVerticesUP 0x82265C98`,
  `BeginVertices 0x822060A0` all stayed at zero, while the game rendered
  frames.
- The PM4 packet-writer census shows where the commands come from: a large
  game cluster at **0x82CCxxxx** (`sub_82CC7888` 13 packet sites, ~30 more
  functions with 2-13 sites) plus the game's render backend at 0x82B9-0x82BA.
- Conclusion: Fable II's engine submits **its own PM4 command stream** and
  calls the kernel `Vd*` API directly (VdInitializeRingBuffer, VdSwap). It
  does not use the statically linked Direct3D device API for rendering.
  Renut has nothing to hook.

Consequences:

1. A deko3d renderer for Fable must sit at the **PM4 command-processor level**
   (replace the SDK's Vulkan graphics backend with a deko3d one), or replace
   the game's own draw pipeline (an engine-level native renderer, far larger
   and game-specific).
2. Scope of the PM4 backend: the SDK's Vulkan backend is ~1,050 KB of source
   plus ~150 KB of headers (graphics_system, command_processor,
   render_target_cache, texture_cache, shared_memory, presenter, shaders).
   A deko3d equivalent is a multi-week/month project, not a port-day task.
3. It is not yet proven that the driver is the bottleneck: at the menu the
   frame costs 0.6-2 s with only ~1.1 cores of CPU and ~1 s/s of GPU fence
   waits; the frame also performs ~418 render-target transfers and 18
   resolves, which a deko3d backend would still have to perform unless the
   render-target emulation is changed. **PC DATAPOINT (2026-11-06): the PC
   build runs the main menu at >60 FPS with the emulated renderer.** The
   emulation itself is therefore cheap; the Switch gap is the Vulkan/NVK
   driver + Tegra execution. A deko3d backend is justified - but first a
   diagnostic experiment measures how much of the Switch frame the ~418
   transfers/resolves cost: cvar `vulkan_skip_render_target_transfers` skips
   them entirely (wrong picture). **RESULT (2026-11-06): menu 1.6 fps -> 12.8
   fps with transfers/resolves skipped.** They are ~87% of the frame. The
   remaining ~77 ms/frame is draw/shader/CPU work that also runs on the same
   GPU under deko3d. Expected deko3d ceiling for the menu is therefore in the
   ~10-20 fps range unless the transfer emulation is redesigned too. (With the
   skip on, starting a new game crashes on unresolved targets - measurement
   build only.)
4. Backend scope measured: the Xenos **common** layer is small
   (command_processor.cpp 66 KB, primitive_processor 80 KB, shared_memory
   23 KB, graphics_system 17 KB), while the Vulkan **execution layer** is the
   ~1 MB a deko3d backend must reimplement: command_processor 336 KB,
   render_target_cache 322 KB, pipeline_cache 175 KB, texture_cache 165 KB,
   deferred_command_buffer 21 KB, shared_memory 18 KB, primitive_processor
   11 KB, shader/graphics_system ~4 KB.
5. **SHADER CHAIN VALIDATED (2026-11-06): 28 of 28 menu shaders compile to
   DKSH.** The automated pipeline is in the repository:
   - the SDK's Vulkan backend dumps each translated shader's SPIR-V
     (`dump_shaders=<dir>`, analysis aid in
     src/graphics/vulkan/shader.cpp);
   - `tools/deko3d/convert_shaders.py` runs SPIRV-Cross (Vulkan GLSL 4.60)
     then uam;
   - `tools/deko3d/deko3d_glsl_fable.py` rewrites what uam cannot take:
       * Vulkan `set = N` qualifiers stripped;
       * separate `texture2DArray` + `sampler` pairs used through
         `sampler2DArray(t, s)` merged into combined samplers (one binding
         each);
       * the `scalar`-layout `XeSystemConstants` block (vec3 at offset 124)
         becomes a read-only std430 SSBO of raw uints, with every member
         access rewritten to typed reads at its exact byte offset - so the
         constant bytes are preserved exactly;
       * unused blocks dropped; `gl_VertexIndex` -> `gl_VertexID`.
   - tools built in WSL: `re/tools/spirv-cross-build/spirv-cross`,
     `re/tools/uam-build/uam` (uam 1.1.0).
   - Result: 28/28 compiled, DKSH median 5,120 bytes, max 52,992.
   Backend consequence: the system constants must be bound as a storage
   buffer (the other constant blocks stay std140 UBOs).

## Original route (kept for the record)

Route: the one sa2-nx documented in `docs/deko3d-renderer-guide.md` (itself
banjo-nx's). Renut, a Direct3D-level renderer, hooks the game's own D3DDevice_*
calls, reads the device state the recompiled game maintains, and replays each
draw through **plume** (deko3d on Switch, Vulkan/D3D12 on PC). No PM4 ring is
parsed and no Vulkan/NVK driver is used in the final Switch build.

## Why (measured on the console, 2026-11-05)

- The emulated Xenos -> NVK path renders the menu at 1.6-2.0 fps: 385 draws,
  5 submits, 18 resolves (4 screen tiles), ~418 render-target transfers/frame.
- `gpu_log_waits`: 432 WAIT_REG_MEM/s, **0 blocked** - not sync-bound.
- Reporting a 720x480 display mode and disabling 2x MSAA barely moved it.
- CPU ~1.8-2.4 cores; frame time scales with draws/fill. The cost is
  per-draw/per-command work in the emulated path, which deko3d removes
  (banjo-nx measured NVK: ~3 us/pipeline bind, 2.4-2.9 us/draw, 1.3-1.6 us
  per descriptor/uniform update).

## Licences / obligations

| Part | Licence | Note |
|---|---|---|
| Renut (`app/src/renut_engine`) | no licence file; author permits reuse in the user's ports | credit reNut / masterspike52 |
| banjo-nx + sa2-nx parts around it | GPL-3.0 | the port tree becomes GPL-3.0 |
| plume + banjo deko3d backend | MIT | as-is |
| uam (deko3d shader compiler) | zlib + MIT + MPL-2.0 | linked into the NRO |
| XenosRecomp (reblue fork) | MIT | build tool |
| ReXGlue SDK | BSD-3-Clause | no-NVK switch patch |

## Phases

### Phase 0 - census (IN PROGRESS)

0. **Fable II's Direct3D = XDK 2.0.7645.0** (string in the decrypted image:
   "Microsoft (R) Xbox 360 Shader Assembler 2.0.7645.0"). That is the SAME
   version as Banjo-Kazooie: Nuts & Bolts, the game Renut was built for:
   N&B's public device, constants, packets and resource headers are
   byte-for-byte identical between those two titles, so Renut's N&B offsets
   should apply nearly verbatim. (SA2 is 2.0.21076 and needed the retarget;
   Fable is expected to need less.)
1. **D3D library block located:** Fable II's function starts form a dense
   block of 565 functions inside 0x82B80000-0x82BB0000 - the same shape as
   SVR2011's D3D block (562 functions, 0x82914828-0x82933070). Vd* call
   anchors land inside it: VdSwap caller at MainRenderLoop_82B9CD68 (the D3D
   present path; the upstream "main loop"/"begin frame" names come from its
   once-per-frame use), ring init VdInitializeRingBuffer/
   VdEnableRingBufferRPtrWriteBack at sub_82B9BA58, VdSetGraphicsInterrupt-
   Callback at 0x82BA0220/0x82BA04A8, VdQueryVideoMode at 0x82B9FD98/
   0x82BA46E0/0x82BA6F08, VdGetCurrentDisplayGamma at 0x82B9DBB8.
2. **One D3D hook already identified:** CreateVertexShader = **0x82B92D48**
   (1.000 instruction-shape match against SVR2011's, 60 insns).
3. SVR2011 (XDK 21076) is a weak shape reference for most other hooks
   (0.1-0.25 ratios: the compiler/XDK drift changes the code too much).
   The correct reference is a **2.0.7645 game, i.e. N&B**; sa2-nx's docs list
   N&B's exact D3D hook addresses. Waiting on whether the user has that
   disc image / extracted XEX; otherwise the remaining ~19 hooks must be
   identified structurally (Vd/XG anchors + call patterns) and verified.
4. Two other Fable notes: the engine does call the D3D inline/ring APIs
   directly (BeginRingBig-style raw packets are likely), and the menu crash
   (sub_822358A8 with r3=0) is independent.
5. **N&B cross-check (same shader-assembler string, 2.0.7645):** only
   **164 of 2,247** Banjo D3D-cluster functions match Fable exactly (~7%).
   Fable links a close but *different* Direct3D runtime build, so N&B's
   addresses cannot be transferred. Confirmed structural hooks so far:
   - DrawIndexedVertices = 0x8221E0F0 (276-instruction prefix identical to
     N&B's, 8 callers incl. Fable's renderer at 0x82FExxxx)
   - DrawVerticesUP = 0x82265C98 (18-ins body identical to N&B's)
   - BeginVertices = 0x822060A0 (called by DrawVerticesUP)
   - SetRingBufferParameters = 0x82B9BA58 (284 ins identical; calls
     VdInitializeRingBuffer / VdEnableRingBufferRPtrWriteBack)
   - CreateVertexShader = 0x82B92D48 (60 ins identical)
   - Swap/present = MainRenderLoop_82B9CD68 (calls VdSwap, VdSetDisplayMode,
     VdPersistDisplay, VdGetSystemCommandBuffer; also calls the wait at
     0x82242628 — the upstream "frame limiter" naming was a misnomer)
   - D3D engine init = 0x82BA0220 (VdInitializeEngines, interrupt callback),
     device ctor path 0x82B69978/0x82B69C88; the device layout offsets seen in
     Fable match N&B's (+10896 ring, +13444 inline ptr, +48 current ptr).
6. **Runtime census build:** with -DFABLE2_D3D_CENSUS=ON the port hooks every
   1,782 sub_ functions in the three candidate D3D clusters and logs calls,
   caller and r3-r7 ranges every 5 s. This is how the remaining Renut hooks
   (KickOff, fences, ClearF, Resolve, CreateDevice, shader registration,
   CreateVertexDeclaration, resource destroy) will be identified: by call
   rate and argument shape during a boot to the menu.
   `tools/d3d_match.py` (from sa2-nx) comparing instruction shapes against
   **SVR2011** as the named reference (same XDK-family Direct3D; different
   immediates/offsets only):
   - decrypted image dumps via `REXGLUE_DUMP_IMAGE`:
     `port/default_image.bin` (Fable) and `re/svr-image/svr_image.bin` (SVR);
   - named reference set: `re/svr_d3d_names.toml` (from svr2011-nx's hooks);
   - then verify each match by its call layout and by reading every device
     offset, naming the function it came from.
3. Map the device block: public device, constants, packets, resource headers,
   fences/flags, bound objects, targets, streams, textures, shaders.

### Phase 1 - Renut on the PC

- Add Renut to the Windows Fable-2-Recomp build (Vulkan/D3D12 back end) with a
  `fable2_renderer = "xenos" | "renut"` switch; keep the emulated renderer as
  the reference.
- `Direct3D_CreateDevice` runs without a GPU (device type 2); the hook supplies
  critical sections, VdGlobalDevice, fence word, timer frequency, presentation
  parameters and InitializeApiState in the hardware path's order.
- KickOff becomes a sink; InsertCallback/Swap/fences hooked; the game's own
  state setters keep writing the device shadows the draws read.
- Shaders by container hash at XGRegister*Shader time. Fable II's shaders live
  in `data/Shaders/ShadersRelease.sbk` and are registered at runtime, so the
  registration hook can dump the containers directly (no bank parser needed).
- Frame pacing per the guide: each Swap released on the game's presentation
  interval grid.

### Phase 2 - shaders for deko3d

- Container dump -> XenosRecomp -> HLSL -> DXC -> SPIR-V -> SPIRV-Cross ->
  GLSL -> passes (deko3d_glsl_opt) -> package.
- Variants by specialization constants; DKSH cache compiled by uam on the
  console (`deko3d_shaders.bin` beside the NRO).
- Release keeps only the shader table in the NRO; GLSL ships in the
  `deko3d_glsl.bin` package made from the player's own copy.

### Phase 3 - deko3d on the Switch

- plume's deko3d back end + uam linked; `renut_gpu_backend = "deko3d"`.
- SDK `REXGLUE_SWITCH_NO_NVK`; the NRO stops linking Mesa.
- Render thread + the sa2 tuning keys (device deltas, store cards, chain
  reuse, NEON compares, batched wakes, core layout). Each has a verify mode.

## Known Fable II risks

- The device layout is a different XDK build; wrong offsets give a wrong
  picture, not a crash. Every offset must name the function it came from.
- Post-process / gamma: Fable II's menu and cutscenes use full-screen passes
  and possibly a display gamma ramp; expect the same fixes SA2 needed.
- Videos (Bink) are guest CPU work and are not affected by the renderer
  change; the intro skip stays.
- The menu crash (`sub_822358A8` called with r3 = 0) is independent of this
  work and must be fixed for long sessions.
