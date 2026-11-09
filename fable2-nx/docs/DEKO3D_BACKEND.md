# Fable II - deko3d backend implementation

Companion to `DEKO3D_PLAN.md` (route, evidence, shader chain). This document is
the implementation map for the new Xenos graphics backend.

## What was validated first

- **Shader chain**: 28/28 dumped menu shaders SPIR-V -> SPIRV-Cross -> Fable
  GLSL rewrite -> uam -> DKSH (see DEKO3D_PLAN.md).
- **deko3d device + swapchain + present**: `tools/deko3d/clear_test/` is a
  standalone NRO (device, graphics queue, two presentable RGBA8 framebuffers,
  `dk::SwapchainMaker` on `nwindowGetDefault()`, static colour pattern). Its
  setup is the code the SDK presenter will use. Run it once on the console
  before integrating.

## SDK integration points

- `include/rex/graphics/graphics_system.h`: `GraphicsSystem` needs
  `CreateProvider(bool)` and `CreateCommandProcessor()`. `SetupPresentation`
  creates the provider with presentation, then `provider->CreatePresenter()`.
- `include/rex/graphics/command_processor.h`: the **common** PM4 parser and
  register state; a backend implements only:
  `IssueSwap`, `SetupContext`, `ShutdownContext`, `LoadShader`, `IssueDraw`,
  `IssueCopy` (six pure virtuals).
- `include/rex/ui/graphics_provider.h` / `presenter.h`: a provider and
  presenter. The presenter base owns the paint thread and guest-output
  mailbox; a backend implements `GetSupportedSurfaceTypes`, `CaptureGuestOutput`,
  `ConnectOrReconnectPaintingToSurfaceFromUIThread`,
  `DisconnectPaintingFromSurfaceFromUIThreadImpl`, `RefreshGuestOutputImpl`,
  `PaintAndPresentImpl`.
- `include/rex/ui/surface_switch.h`: `ViNWindowSurface` exposes the libnx
  `NWindow` for the deko3d swapchain (`Window::GetNativeWindowHandle()` is
  also available).
- Build flags: `REX_HAS_VULKAN` / `REX_HAS_D3D12` are defined by
  `src/core/CMakeLists.txt` and `src/system/CMakeLists.txt` from
  `REXGLUE_USE_*`; the plugin picks a backend in
  `src/graphics/plugin_main.cpp` (`rex_gpu_create`, backend string).
- The Vulkan backend files are the structural reference:
  `src/graphics/vulkan/{graphics_system,command_processor,pipeline_cache,
  render_target_cache,texture_cache,shared_memory,deferred_command_buffer,
  shader}.cpp` (~1 MB total). The common layer
  (`src/graphics/{command_processor,primitive_processor,shared_memory,
  graphics_system}.cpp`) stays untouched.

## Milestones

### D0 - deko3d presentation in the SDK (clear screen) - IMPLEMENTED, BUILDING

1. `REXGLUE_USE_DEKO3D` CMake option (Switch only) -> `REX_HAS_DEKO3D=1` in
   rexcore/rexruntime; `src/graphics/deko3d/*` and `src/ui/deko3d/*` added to
   the builds; the switch target links `deko3d`.
2. `ui::deko3d::Deko3DProvider` + `Deko3DPresenter`:
   - `deko3d_presenter.cpp` creates the device/queue, two presentable RGBA8
     framebuffers and a swapchain on the `ViNWindowSurface`'s NWindow (the code
     validated by tools/deko3d/clear_test), and presents a clear pattern.
   - guest output refresh is accepted and ignored for now; the swap call from
     the guest asks the presenter for a refresh, which schedules the paint.
3. `graphics::deko3d::Deko3DGraphicsSystem` + `Deko3DCommandProcessor`:
   - `IssueSwap` calls `Presenter::RefreshGuestOutput(...)` with a no-op
     refresher; the presenter paints the clear pattern.
   - `LoadShader` returns nullptr, `IssueDraw`/`IssueCopy` return true, so the
     guest ring keeps advancing and the game logic runs without drawing.
4. `plugin_main.cpp` selects deko3d for `"deko3d"` and for `"any"` when it is
   compiled (before D3D12/Vulkan).
5. Success criterion: the game boots to the menu with the deko3d clear pattern
   visible while guest logic runs.
6. **STATUS: builds and links (2026-11-06).** The NRO is 98,855,109 bytes and
   selects deko3d for the `"xenos"` plugin's `"any"` backend on Switch.
7. **D0 PASSED ON HARDWARE (2026-11-06):** the deko3d clear pattern appears on
   the console and the guest runs at full speed (audio at speed, CPU ~185%).
   Log: `[deko3d] presenter connected: 1280x720`; paint mode switches between
   the game thread and the UI thread as designed. The profiler showed 0.0 fps
   only because the deko3d swap did not increment the SDK's frame counter; it
   now calls RexSwitchPerfCount(0) like the Vulkan path.

### D1 - guest memory + commands

- D1a (implemented): the swap path hands the guest front buffer's host pointer
  to the presenter (`Deko3DGuestOutputRefreshContext`); the presenter stages it
  into a CPU-cached deko3d buffer, flushes it, and copies it into the acquired
  swapchain image with `dkCmdBufCopyBufferToImage`. Layout assumed: linear
  8_8_8_8, pitch = width * 4. Before the first guest swap the static pattern is
  shown. With draws still no-ops the guest buffer is usually black - this
  milestone proves the guest-output path, not the picture.
- FIRST ATTEMPT CRASHED (first swap): the front buffer (guest 0x1ECFF000) was
  not committed and lives in a non-physical heap, so the CPU read faulted and
  the runtime reported an unhandled guest access violation. The read is now
  gated on `LookupHeap(...) == kGuestPhysical` and a committed allocation; until
  the GPU resolve path writes the buffer, the presenter keeps the pattern.
  The real guest output comes from the D2 resolve path, not from CPU-reading an
  address the guest has not written yet.
- Next: a 512 MB deko3d buffer over guest physical memory (same layout as the
  Vulkan buffer), ring/read-pointer/event handling is already the common
  command processor's.

### D2 - textures and render targets (the hard half)

**Status: DONE on hardware (2026-11-08).** Measured with the game at the main
menu and through character select:

- **29.6-29.9 fps** (the guest's own 30 Hz cap) with GPU Commands at ~2.5% CPU,
  versus 1.6 fps on the Vulkan/NVK path for the same scene.
- Game logic, audio, saves and menu input all run; the host render target is
  presented (white clear visible, then black because draws are not issued yet).
- Shader pack loads: `deko3d: shader pack ... loaded (28 shaders)`.

What is implemented:
- Shared `Deko3DDevice`/queue owned by the provider, used by the presenter and
  the Xenos command processor.
- `Deko3DSharedMemory`: one 512 MB deko3d block mirroring guest physical memory
  (CPU-uncached, so GPU writes are visible after a fence).
- `Deko3DRenderTargetCache`: host color/depth images per EDRAM key, MSAA images
  with 1x resolve images, color/depth clears, and presentation of the target
  that resolved to the swap front-buffer address.
- `Deko3DTextureCache`: deko3d image creation and base-level CPU untiling/upload
  for RGBA8 and BCn textures.

Hardware lessons recorded here because each cost a test cycle:
- Fable submits resolves as **draw packets with `RB_MODECONTROL.edram_mode ==
  kCopy`**; a backend that only implements `IssueCopy` never sees them.
- `DkCopyBuf.rowLength` is **bytes per row** and `imageHeight` is **total
  bytes** (`rowLength * rows`), not pixels/rows. Wrong units stall the 2D engine
  with no queue error.
- `copyImageToBuffer` from a render target into the guest mirror **stalls this
  queue** (queue error state 0, fence never signals). Presentation therefore
  copies the host target image directly; the guest-buffer resolve is deferred to
  a compute/DKSH pass.
- A **second graphics queue does not execute** its command lists here: clears
  must be submitted on the presentation queue, ordered before the present copy.
- Swapchain images need `DkImageFlags_Usage2DEngine` for 2D-engine copies, and
  three images plus a copy fence (never `dkQueueWaitIdle`) per the SVR2011 guide.

### D3 - draws

Verified facts that shape the implementation:

- **This deko3d has no pipeline-state-object API.** There is no
  `dkGraphicsPipelineMaker`; plume's working backend binds everything
  dynamically per draw: `bindShaders`, `bindRasterizerState`,
  `bindMultisampleState`, `bindColorState`, `bindColorWriteState`,
  `bindBlendStates`, `bindDepthStencilState`, `setViewports`, `setScissors`,
  then `draw`/`drawIndexed`. So no PSO cache is needed.
- **Vertex fetching happens in the shader.** The translated vertex shaders read
  `XeSharedMemory` with `gl_VertexID` and the fetch constants, so there are no
  host vertex buffers or vertex attribute layouts to build.
- Binding model (after moving shared memory off binding 0):
  - storage buffer 0 = `XeSystemConstants` (126 words)
  - storage buffer 1 = `XeSharedMemory` (the 512 MB mirror)
  - uniform buffer 2 = `XeFloatConstants`, uniform buffer 4 = `XeFetchConstants`
  - `sampler2DArray` textures at bindings 0..N
- `SpirvShaderTranslator::SystemConstants` maps 1:1 onto `dk_sys[]`. Verified
  word indices: 2 `vertex_index_endian`, 3 `line_loop_closing_index`,
  4 `vertex_base_index`, 72..75 `color_exp_bias`, 124/125
  `vertex_index_min`/`max`. The shaders declare 126 words because the
  tessellation constants (126/127) are unused.
- Shader lookup key: `XXH3_64bits(ucode_dwords, dword_count * 4)`, formatted
  `<ps|vs>_%016X_%016X` with the translator modification. The modification is
  reproduced at runtime (`GetVertexShaderModification` /
  `GetPixelShaderModification`), so the exact variant is picked; a miss falls
  back to any variant of the same microcode and logs the missing key.
- Pack coverage at the menu is nearly complete. Missing so far:
  `vs_AB1E86137A0240E8`, `vs_D5E12A97B3A84881`, `ps_93E7F7B5D05626AA` - refresh
  by running the **Switch** build with `gpu_backend = "vulkan"` and
  `dump_shaders` set (the PC build uses D3D12 and dumps nothing), then
  `convert_shaders.py` and `pack_shaders.py`.
- **The clip-space conventions already match.** The device is created with
  deko3d's defaults, `DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne`,
  which map clip-space y = +1 to the top row with depth 0...1 - Vulkan's, and
  what Xenia's SPIR-V and `draw_util::GetHostViewportInfo(origin_bottom_left =
  false)` produce. No viewport flip and no `ndc_scale` sign change are needed
  (SVR2011 needs one only because its shaders come from DXBC via
  `-fvk-invert-y`).
- **The texture binding table has to travel with the pack.** uam drops the
  SPIR-V names, and the Vulkan backend numbers texture and sampler descriptors
  separately per stage while deko3d has one texture namespace per stage holding
  combined image+sampler handles. `pack_shaders.py` therefore parses the
  converter's `dk_xe_texture<N>_<dim>_<u|s>_xe_sampler<M>_<fff>` names out of
  the generated GLSL and stores `(binding, texture fetch constant, sampler
  fetch constant, dimension, signedness, mag/min/mip, aniso)` per shader
  (`F2DKSH2`).

What is implemented:

- `Deko3DShaderCodeArena`: 4 MiB `DkMemBlockFlags_Code` blocks, DKSH copied in
  at 256-byte alignment, `per_warp_scratch_sz` from the DKSH program header
  checked against the queue's (raised to 64 KiB - the 2 KiB default is far
  below what Xenia's shaders ask for).
- `Deko3DShader`: microcode holder plus a per-modification cache of loaded
  `dk::Shader` variants and their binding tables.
- `Deko3DPrimitiveProcessor`: configured `(full 32-bit indices, no triangle
  fans, no line loops, no quad lists, native point sprites, no native rectangle
  lists)`. Fans, loops and quads are converted with the built-in index buffer so
  the host vertex shader type stays `kVertex` - the only one in the pack.
  Rectangle lists need a vertex-shader-expansion variant and are reported
  missing until a dump includes one.
- `Deko3DGpuRing`: one CPU-cached block per kind (constants 16 MiB, descriptors
  4 MiB, converted indices 8 MiB), bump-allocated, flushed with one
  `flushCpuCache` before the draw, recycled when a new command list opens.
- Constant upload: system constants (SSBO 0), tightly packed float constants
  (UBO 1 vertex / UBO 2 pixel, from `ConstantRegisterMap::float_bitmap`),
  bool+loop (UBO 3) and fetch constants (UBO 4). Consecutive draws that repeat
  a buffer reuse its ring slice.
- `UpdateSystemConstantValues` ported from the Vulkan backend with the
  fragment-shader-interlock paths dropped (this backend is always host render
  targets): flags, index endian/base/reset, user clip planes, ndc scale/offset,
  point sizes, texture signs, alpha test and alpha-to-mask, colour exponent
  bias.
- State: rasterizer (cull, front face, polygon mode, depth clamp for
  clip-disabled draws, polygon offset via `setDepthBias`), multisample (MSAA
  mode from `RB_SURFACE_INFO`, alpha-to-coverage), per-target blend and write
  masks, depth/stencil including the back-face ops and `setStencil`,
  viewport/scissor from `GetHostViewportInfo`/`GetScissor`, primitive restart.
  Alpha test is left to the pixel shader (`alphaCompareOp = Always`), matching
  the translated shaders.
- Textures: per draw, an image and a sampler descriptor set in the ring, bound
  with `bindImageDescriptorSet`/`bindSamplerDescriptorSet`, and one
  `dkMakeTextureHandle(i, i)` per binding via `bindTextures` (pixel shader
  slots first, then vertex). Samplers come from the fetch constants combined
  with the binding's filter overrides; LOD bias stays 0 because the shader
  applies it. An invalid fetch constant binds a 1x1 opaque-white placeholder.
- Draws: non-indexed, guest-DMA indexed (the guest's own big-endian buffer is
  bound from the mirror and the shader swaps, exactly as on Vulkan), and
  host-converted / built-in indexed.
- All of a frame's clears, draws and resolves record into one command list
  submitted at the swap; after 3000 draws it is submitted and awaited so the
  8 MiB command memory cannot overflow.

Not implemented yet:

- Memexport draws are skipped (counted and logged by reason).
- EDRAM tile-ownership transfers between live render targets are not performed;
  a request is logged once.
- Depth resolve copies, and the guest-buffer colour resolve (still presented
  from the host target image directly).
- Mip levels, 3D and cube texture uploads beyond the base level.

### Known guest crash (independent of rendering)

Entering the game world dies with `Unhandled guest access violation: read of
guest 0x00000024` - the null-deref at `sub_822358A8` (r3 = 0) noted since the
Vulkan bring-up. The PC reference tree's `sdk_mainmenu_crash_fix.patch`
(`AllocFixed` must fail a re-reserve of an already-reserved range) is already
present in this SDK, so that particular pool corruption is not the cause here.

- `deko3d/TextureCache`: Xenos tiled formats -> deko3d images (swizzle,
  endianness, mips, signed/unsigned variants), invalidation on guest writes.
- `deko3d/RenderTargetCache`: EDRAM emulation, host render target surfaces,
  ownership transfers and resolve clears. Must **batch** transfers and use
  native copies/blits (the Vulkan path's ~187 dispatches/frame cost ~87% of
  the Switch frame; see DEKO3D_PLAN.md).
- `deko3d/PipelineCache`: deko3d pipelines from the register state; DKSH from
  the validated shader chain (SPIRV-Cross + the Fable GLSL rewrite + uam,
  linked into the NRO). Constant blocks: system constants as a read-only
  storage buffer, float/bool/fetch constants as std140 UBOs.

### D3b - correctness pass

- Verify the menu and then gameplay against the PC build frame by frame.
- Add the missing shader variants to the pack (re-dump from the Switch Vulkan
  build) and re-check the skip-reason counts in the log.
- Decide whether memexport draws and EDRAM ownership transfers are needed for
  the world, and implement them if so.

### D4 - performance

- Match or beat the emulated Vulkan path's draw-only ceiling (~13 fps at the
  menu) by removing NVK's per-draw/transfer overhead; profile with the SDK's
  Switch perf counters and `gpu_log_waits`.

## Notes / traps

- Keep the Vulkan backend buildable and selectable until D3; the game must
  remain comparable on the PC and Switch.
- The guest memory window cannot use read-only protections on Horizon
  (`svcSetProcessMemoryPermission` -> 0xD401), so coherence stays as in the
  Vulkan backend.
- deko3d images need their memory blocks with `DkMemBlockFlags_Image` and
  aligned offsets (`layout.getAlignment()`); the clear test shows the pattern.
- The presenter base class has a paint thread and a guest-output mailbox; D0
  should return the clear frame from `PaintAndPresentImpl` and `false` from
  `RefreshGuestOutputImpl`.
