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

### D3 - draws

- Primitive processing (the common primitive processor already expands
  points/quads); vertex fetch and index buffers from shared memory;
  memexport if the game uses it.
- Verify the menu and then gameplay against the PC build frame by frame.

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
