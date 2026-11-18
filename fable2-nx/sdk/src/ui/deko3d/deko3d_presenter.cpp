/**
 * @file        ui/deko3d/deko3d_presenter.cpp
 * @brief       Presenter for the deko3d backend: device, swapchain and present
 *              on the libnx default NWindow.
 *
 * The device/swapchain setup is the code validated by
 * tools/deko3d/clear_test (green pattern on hardware). For now the frame is a
 * constant colour pattern; the guest output will be composited here in the
 * next milestones.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <switch.h>
#include <deko3d.hpp>

#include <rex/graphics/deko3d/fence_wait.h>  // NoteGpuSubmit (run-060 age retirement)
#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>
#include <rex/ui/deko3d/deko3d_presenter.h>
#include <rex/ui/surface_switch.h>

namespace rex::ui::deko3d {

namespace {

constexpr unsigned kNumFramebuffers = 3;
constexpr uint32_t kCmdSize = 0x1000;
// Present-copy command slots (age-based retirement; see PaintAndPresentImpl).
constexpr uint32_t kPresentCmdSlots = 16;

}  // namespace

struct Deko3DPresenter::Impl {
  std::mutex mutex;
  Deko3DDevice* device = nullptr;
  dk::UniqueSwapchain swapchain;
  dk::UniqueCmdBuf cmdbuf;
  dk::UniqueCmdBuf gpu_cmdbuf;
  dk::UniqueMemBlock fb_mem;
  dk::UniqueMemBlock cmd_mem;
  dk::UniqueMemBlock gpu_cmd_mem;
  dk::UniqueMemBlock upload_mem;
  DkCopyBuf upload_buf{};
  std::array<dk::Image, kNumFramebuffers> framebuffers{};
  std::array<DkCmdList, kNumFramebuffers> bind_cmdlists{};
  std::array<DkCmdList, kNumFramebuffers> guest_copy_cmdlists{};
  DkCmdList pattern_cmdlist{};
  bool copy_pending = false;
  bool connected = false;
  uint32_t width = 0;
  uint32_t height = 0;
  const uint8_t* guest_pixels = nullptr;
  uint32_t guest_width = 0;
  uint32_t guest_height = 0;
  uint32_t guest_pitch = 0;
  DkGpuAddr guest_gpu_pixels = 0;
  uint32_t guest_gpu_pitch = 0;
  dk::Image guest_target_image{};
  uint32_t guest_target_width = 0;
  uint32_t guest_target_height = 0;
  bool has_guest_target = false;
  DkFence* render_fence = nullptr;
  // Run-059 per-present telemetry: how far the swapchain is behind the guest.
  uint32_t present_count = 0;
  uint32_t present_cmd_slot = 0;
  // Run-062: a fence signalled on the render queue after a present copy, polled
  // (never waited) so the log answers "does a fence here ever signal?" on the
  // queue that demonstrably executes. Run 061 only ever asked at boot, where a
  // queue that had never run anything could not answer.
  dk::Fence present_fence{};
  uint32_t present_fence_at = 0;
  uint32_t present_fence_result = 0;  // 0 = pending, 1 = signalled (telemetry only)
  uint32_t present_fence_delay = 0;
  uint32_t refresh_epoch = 0;
  uint32_t present_at_last_refresh = 0;
  std::chrono::steady_clock::time_point last_refresh{};
  // Run-069 (F1): read the guest output image the present copy reads from, at
  // present 30 and 150, and print checksum + sample grid + blank/content verdict.
  // Own cmdbuf/mem (never aliases the present-copy slots); CpuUncached readback
  // so the CPU reads DRAM, not its own dirty 0xAA pre-fill (run-068 lesson).
  dk::UniqueCmdBuf readback_cmdbuf;
  dk::UniqueMemBlock readback_cmd_mem;
  dk::UniqueMemBlock readback_mem;
  uint32_t guest_readbacks_done = 0;

  // Run-069 (F1): read back the guest output image the present copy reads from
  // (the branch=1 surface) and print a checksum, an 8x8 sample grid and an
  // explicit blank/content verdict. Uniform clear colour -> the guest never drew
  // into it (fault upstream in draw/target binding); varying content -> the
  // present copy/swapchain is at fault. Called from PaintAndPresentImpl AFTER
  // the frame's render list and the present copy are submitted, so queue order
  // makes these the exact pixels the present copy reads. The caller already
  // holds device->submit_mutex() - do NOT take it again here.
  void ReadBackGuestOutputLocked(dk::Queue queue, uint32_t present_number) {
    const uint32_t w = guest_target_width;
    const uint32_t h = guest_target_height;
    if (!w || !h) {
      return;
    }
    const uint32_t row_bytes = w * 4;  // the resolved front buffer is RGBA8
    const uint64_t bytes = uint64_t(row_bytes) * h;
    const uint32_t mem_size = uint32_t((bytes + DK_MEMBLOCK_ALIGNMENT - 1) &
                                       ~uint64_t(DK_MEMBLOCK_ALIGNMENT - 1));
    if (!readback_mem) {
      readback_cmdbuf = dk::CmdBufMaker{device->device()}.create();
      readback_cmd_mem =
          dk::MemBlockMaker{device->device(), kCmdSize}
              .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
              .create();
      readback_mem =
          dk::MemBlockMaker{device->device(), mem_size}
              .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
              .create();
      if (!readback_cmdbuf || !readback_cmd_mem || !readback_mem) {
        REXLOG_WARN("[guest-readback] allocation failed - no verdict this run");
        return;
      }
      std::memset(readback_mem.getCpuAddr(), 0xAA, mem_size);
    }
    if (bytes > readback_mem.getSize()) {
      REXLOG_WARN("[guest-readback] #{}: target {}x{} outgrew the readback block - skipped",
                  present_number, w, h);
      return;
    }
    readback_cmdbuf.clear();
    readback_cmdbuf.addMemory(readback_cmd_mem, 0, kCmdSize);
    readback_cmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dk::ImageView src_view{guest_target_image};
    DkCopyBuf dst{};
    dst.addr = readback_mem.getGpuAddr();
    dst.rowLength = row_bytes;
    dst.imageHeight = h;
    const DkImageRect rect{0, 0, 0, w, h, 1};
    readback_cmdbuf.copyImageToBuffer(src_view, rect, dst, 0);
    const DkCmdList list = readback_cmdbuf.finishList();
    dk::Fence fence{};
    queue.submitCommands(list);
    queue.signalFence(fence, true);
    queue.flush();  // kick now: the readback must not wait for the next present
    rex::graphics::deko3d::NoteGpuSubmit();
    if (fence.wait(250000000) != DkResult_Success) {
      REXLOG_WARN("[guest-readback] #{}: fence TIMEOUT (250 ms) - pixels NOT trustworthy, "
                  "no verdict", present_number);
      return;
    }
    const uint8_t* p = static_cast<const uint8_t*>(readback_mem.getCpuAddr());
    uint32_t fnv = 2166136261u;
    uint8_t min_b = 0xFF, max_b = 0x00;
    uint64_t zero_bytes = 0;
    for (uint64_t i = 0; i < bytes; ++i) {
      fnv ^= p[i];
      fnv *= 16777619u;
      if (p[i] < min_b) min_b = p[i];
      if (p[i] > max_b) max_b = p[i];
      if (!p[i]) ++zero_bytes;
    }
    uint32_t samples[8][8];
    for (uint32_t gy = 0; gy < 8; ++gy) {
      for (uint32_t gx = 0; gx < 8; ++gx) {
        const uint32_t x = gx * w / 8 + w / 16;
        const uint32_t y = gy * h / 8 + h / 16;
        const uint8_t* px = p + (uint64_t(y) * row_bytes) + x * 4;
        samples[gy][gx] = uint32_t(px[0]) | (uint32_t(px[1]) << 8) |
                          (uint32_t(px[2]) << 16) | (uint32_t(px[3]) << 24);
      }
    }
    uint32_t distinct = 0;
    for (uint32_t i = 0; i < 64; ++i) {
      bool seen = false;
      for (uint32_t j = 0; j < i; ++j) {
        if (samples[i / 8][i % 8] == samples[j / 8][j % 8]) { seen = true; break; }
      }
      if (!seen) ++distinct;
    }
    char grid[8 * 8 * 9 + 8];
    size_t off = 0;
    for (uint32_t gy = 0; gy < 8; ++gy) {
      for (uint32_t gx = 0; gx < 8; ++gx) {
        off += size_t(std::snprintf(grid + off, sizeof(grid) - off, "%08X ", samples[gy][gx]));
      }
      if (gy < 7) grid[off++] = '|';
    }
    grid[off] = '\0';
    const char* verdict =
        min_b == max_b
            ? "BLANK - the whole surface is one uniform byte value: the guest never drew into "
              "it (fault upstream in draw/target binding)"
            : (distinct >= 2
                   ? "CONTENT - the guest output has varying pixels: if the screen is blank the "
                     "fault is in the present copy / swapchain blit"
                   : "NEAR-BLANK - bytes vary but the 8x8 grid is uniform; inspect the grid");
    REXLOG_WARN("[guest-readback] #{} {}x{}: fnv1a={:08X} min={:02X} max={:02X} zero={}/{} "
                "first=[{:02X}{:02X}{:02X}{:02X} {:02X}{:02X}{:02X}{:02X}] grid=[{}] verdict={}",
                present_number, w, h, fnv, min_b, max_b, zero_bytes, bytes,
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], grid, verdict);
  }

  void ResetLocked() {
    if (device) {
      std::lock_guard dev_lock(device->submit_mutex());
      if (device->queue()) {
        device->queue().waitIdle();
      }
    }
    swapchain = nullptr;
    cmdbuf = nullptr;
    gpu_cmdbuf = nullptr;
    fb_mem = nullptr;
    cmd_mem = nullptr;
    gpu_cmd_mem = nullptr;
    upload_mem = nullptr;
    connected = false;
      width = height = 0;
    guest_pixels = nullptr;
    guest_width = guest_height = guest_pitch = 0;
    guest_gpu_pixels = 0;
    guest_gpu_pitch = 0;
    guest_target_image = dk::Image{};
    guest_target_width = guest_target_height = 0;
    has_guest_target = false;
    render_fence = nullptr;
  }

  bool CreateLocked(NWindow* nw, uint32_t w, uint32_t h) {
    if (!device || !device->valid()) {
      REXLOG_ERROR("[deko3d] no shared device for the presenter");
      return false;
    }
    const dk::Device dk_device = device->device();

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{dk_device}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                  DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(w, h)
        .initialize(layout);
    const uint64_t fb_size = layout.getSize();
    const uint32_t fb_align = layout.getAlignment();
    const uint64_t fb_stride = (fb_size + fb_align - 1) & ~uint64_t(fb_align - 1);
    fb_mem = dk::MemBlockMaker{dk_device, uint32_t(fb_stride * kNumFramebuffers)}
                 .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                 .create();
    if (!fb_mem) {
      REXLOG_ERROR("[deko3d] framebuffer memory allocation failed");
      return false;
    }
    std::array<DkImage const*, kNumFramebuffers> fb_array;
    for (unsigned i = 0; i < kNumFramebuffers; ++i) {
      framebuffers[i].initialize(layout, fb_mem, i * fb_stride);
      fb_array[i] = &framebuffers[i];
    }

    cmd_mem = dk::MemBlockMaker{dk_device, kCmdSize * (kNumFramebuffers * 2 + 2)}
                  .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                  .create();
    if (!cmd_mem) {
      REXLOG_ERROR("[deko3d] command memory allocation failed");
      return false;
    }
    cmdbuf = dk::CmdBufMaker{dk_device}.create();
    if (!cmdbuf) {
      REXLOG_ERROR("[deko3d] command buffer creation failed");
      return false;
    }
    gpu_cmd_mem = dk::MemBlockMaker{dk_device, kCmdSize * kPresentCmdSlots}
                      .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                      .create();
    gpu_cmdbuf = dk::CmdBufMaker{dk_device}.create();
    if (!gpu_cmd_mem || !gpu_cmdbuf) {
      REXLOG_ERROR("[deko3d] GPU present command resources creation failed");
      return false;
    }
    for (unsigned i = 0; i < kNumFramebuffers; ++i) {
      cmdbuf.addMemory(cmd_mem, i * kCmdSize, kCmdSize);
      dk::ImageView color_target{framebuffers[i]};
      cmdbuf.bindRenderTargets(&color_target);
      bind_cmdlists[i] = cmdbuf.finishList();
    }

    // Staging buffer for the guest front buffer and the per-slot copy list
    // that puts it into the swapchain image.
    const uint32_t upload_size = w * h * 4;
    upload_mem = dk::MemBlockMaker{dk_device, upload_size}
                     .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
                     .create();
    if (!upload_mem) {
      REXLOG_ERROR("[deko3d] guest output staging allocation failed");
      return false;
    }
    upload_buf.addr = upload_mem.getGpuAddr();
    // DkCopyBuf.rowLength is bytes per row and imageHeight is the total byte
    // size (rowLength * rows), matching plume's deko3d backend.
    upload_buf.rowLength = w * 4;
    upload_buf.imageHeight = w * 4 * h;
    for (unsigned i = 0; i < kNumFramebuffers; ++i) {
      cmdbuf.addMemory(cmd_mem, (kNumFramebuffers + i) * kCmdSize, kCmdSize);
      dk::ImageView dst_view{framebuffers[i]};
      const DkImageRect rect{0, 0, 0, w, h, 1};
      cmdbuf.copyBufferToImage(upload_buf, dst_view, rect);
      guest_copy_cmdlists[i] = cmdbuf.finishList();
    }

    // Placeholder presentation: a recognisable static pattern until the guest
    // output is composited here.
    cmdbuf.addMemory(cmd_mem, kNumFramebuffers * 2 * kCmdSize, kCmdSize);
    cmdbuf.setScissors(0, {{0, 0, w, h}});
    cmdbuf.clearColor(0, DkColorMask_RGBA, 0.03f, 0.05f, 0.09f, 1.0f);
    const uint32_t box = (w < h ? w : h) / 3;
    const uint32_t box_x = w / 2 - box / 2;
    const uint32_t box_y = h / 2 - box / 2;
    cmdbuf.setScissors(0, {{box_x, box_y, box, box}});
    cmdbuf.clearColor(0, DkColorMask_RGBA, 0.12f, 0.55f, 0.25f, 1.0f);
    cmdbuf.setScissors(0, {{box_x, box_y + box / 2, box, box / 8}});
    cmdbuf.clearColor(0, DkColorMask_RGBA, 0.9f, 0.9f, 0.9f, 1.0f);
    pattern_cmdlist = cmdbuf.finishList();

    swapchain = dk::SwapchainMaker{dk_device, nw, fb_array}.create();
    if (!swapchain) {
      REXLOG_ERROR("[deko3d] swapchain creation failed");
      return false;
    }

    width = w;
    height = h;
    connected = true;
    REXLOG_INFO("[deko3d] presenter connected: {}x{}", w, h);
    return true;
  }
};

Deko3DPresenter::Deko3DPresenter(Deko3DDevice* device,
                                 HostGpuLossCallback host_gpu_loss_callback)
    : Presenter(std::move(host_gpu_loss_callback)), impl_(std::make_unique<Impl>()) {
  impl_->device = device;
}

Deko3DPresenter::~Deko3DPresenter() {
  // The paint thread calls PaintAndPresentImpl, which belongs to this class.
  ShutdownPaintThread();
  std::lock_guard lock(impl_->mutex);
  impl_->ResetLocked();
}

Surface::TypeFlags Deko3DPresenter::GetSupportedSurfaceTypes() const {
  return Surface::kTypeFlag_ViNWindow;
}

bool Deko3DPresenter::CaptureGuestOutput(RawImage& image_out) {
  (void)image_out;
  return false;
}

Presenter::SurfacePaintConnectResult
Deko3DPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
    bool was_paintable, bool& is_vsync_implicit_out) {
  (void)was_paintable;
  if (new_surface.GetType() != Surface::kTypeIndex_ViNWindow) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }
  NWindow* nw = static_cast<ViNWindowSurface&>(new_surface).window();
  if (!nw || !new_surface_width || !new_surface_height) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }

  std::lock_guard lock(impl_->mutex);
  if (impl_->connected && impl_->width == new_surface_width &&
      impl_->height == new_surface_height) {
    is_vsync_implicit_out = false;
    return SurfacePaintConnectResult::kSuccessUnchanged;
  }
  impl_->ResetLocked();
  if (!impl_->CreateLocked(nw, new_surface_width, new_surface_height)) {
    impl_->ResetLocked();
    return SurfacePaintConnectResult::kFailure;
  }
  is_vsync_implicit_out = false;
  return SurfacePaintConnectResult::kSuccess;
}

void Deko3DPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  std::lock_guard lock(impl_->mutex);
  impl_->ResetLocked();
}

bool Deko3DPresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  (void)mailbox_index;
  (void)frontbuffer_width;
  (void)frontbuffer_height;
  std::lock_guard lock(impl_->mutex);
  if (!impl_->connected) {
    return false;
  }
  Deko3DGuestOutputRefreshContext context(is_8bpc_out_ref);
  const bool refreshed = refresher(context);
  if (refreshed) {
    impl_->refresh_epoch++;
    impl_->present_at_last_refresh = impl_->present_count;
    impl_->last_refresh = std::chrono::steady_clock::now();
    if (context.has_target()) {
      impl_->guest_pixels = nullptr;
      impl_->guest_gpu_pixels = 0;
      impl_->guest_gpu_pitch = 0;
      impl_->guest_target_image = context.target_image();
      impl_->guest_target_width = context.target_width();
      impl_->guest_target_height = context.target_height();
      impl_->has_guest_target = true;
    } else if (context.gpu_pixels()) {
      impl_->guest_pixels = nullptr;
      impl_->guest_gpu_pixels = context.gpu_pixels();
      impl_->guest_gpu_pitch = context.gpu_pitch();
      impl_->guest_target_image = dk::Image{};
      impl_->has_guest_target = false;
    } else {
      impl_->guest_pixels = context.pixels();
      impl_->guest_gpu_pixels = 0;
      impl_->guest_gpu_pitch = 0;
      impl_->guest_target_image = dk::Image{};
      impl_->has_guest_target = false;
    }
    impl_->guest_width = context.width();
    impl_->guest_height = context.height();
    impl_->guest_pitch = context.pitch();
    impl_->render_fence = context.render_fence();
  }
  return refreshed;
}

Presenter::PaintResult Deko3DPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  (void)execute_ui_drawers;
  std::lock_guard lock(impl_->mutex);
  if (!impl_->connected || !impl_->device) {
    return PaintResult::kNotPresented;
  }
  // HOME/sleep/capture can stop the display consuming frames; queueing into
  // the window then blocks the game thread. Pause presentation until focus is
  // back, as the SVR2011 deko3d backend does.
  if (appletGetFocusState() != AppletFocusState_InFocus) {
    return PaintResult::kNotPresented;
  }
  // Run-069: bounded wait on the previous frame's present fence, every frame.
  // Run 068 proved fences signal in ~5 ms once the submission is kicked
  // (presentImage flushes), so this wait returns promptly and paces the frame;
  // the old permanent give-up ("NOT signalled after 60 presents -> never wait
  // again") rested on the retracted "fences never signal" artefact and is
  // deleted. A timeout is logged and the frame still presents - a lost signal
  // must not freeze the display.
  if (impl_->present_count > 0) {
    static rex::graphics::deko3d::FenceWaitStats present_wait_stats;
    if (rex::graphics::deko3d::WaitFenceForRetirement(impl_->present_fence, "present copy",
                                                      present_wait_stats)) {
      rex::graphics::deko3d::NoteFramePresented();
    }
  }

  // One-shot telemetry: the first present whose fence is observed signalled.
  if (impl_->present_fence_result == 0 && impl_->present_count > 0) {
    if (impl_->present_fence.wait(0) == DkResult_Success) {
      impl_->present_fence_result = 1;
      impl_->present_fence_delay = impl_->present_count - impl_->present_fence_at;
      REXLOG_WARN("[present-fence] SIGNALLED on the render queue at present #{} ({} frame(s) "
                  "after the copy was submitted) - fences work on this queue",
                  impl_->present_count, impl_->present_fence_delay);
    }
  }

  // Run-069: print the frame-path upload probe as soon as its witness allows,
  // independent of whether any later upload or texture-binding draw happens.
  rex::graphics::deko3d::PollFrameUploadProbe();
  std::lock_guard dev_lock(impl_->device->submit_mutex());
  dk::Queue queue = impl_->device->queue();
  {
    // Run-066: the submit sites compared. This one demonstrably executes; the
    // texture cache's standalone site does not (run 065), which is why uploads now
    // ride this frame list instead.
    static bool logged_once = false;
    if (!logged_once) {
      logged_once = true;
      REXLOG_WARN("[submit] present site=presenter queue=0x{:X} device=0x{:X} fence={}",
                  uint64_t(queue), uint64_t(impl_->device->device()),
                  static_cast<const void*>(&impl_->present_fence));
    }
  }
  const int slot = queue.acquireImage(impl_->swapchain);
  queue.submitCommands(impl_->bind_cmdlists[slot]);
  // The render-target clears are submitted on this same queue before the
  // presenter runs, so queue order already guarantees they are done; a second
  // graphics queue does not execute on this device.
  impl_->render_fence = nullptr;

  const bool has_target = impl_->has_guest_target && impl_->guest_target_width &&
                          impl_->guest_target_height && impl_->guest_width &&
                          impl_->guest_height;
  const bool has_gpu_output = impl_->guest_gpu_pixels && impl_->guest_width &&
                              impl_->guest_height && impl_->guest_gpu_pitch;
  const bool has_guest_output = impl_->guest_pixels && impl_->guest_width &&
                                impl_->guest_height && impl_->guest_pitch;
  // Run 054: name the branch that feeds the swapchain. Branch 4 (pattern) means
  // no guest output was ever attached and the screen can only be the pattern.
  const uint32_t branch_log =
      has_target ? 1u : has_gpu_output ? 2u : has_guest_output ? 3u : 4u;
  {
    static uint32_t last_branch = 0xFFFFFFFFu;
    const uint32_t branch = branch_log;
    if (branch != last_branch) {
      last_branch = branch;
      REXLOG_WARN("[present] branch={} target={}x{} gpu=0x{:016X} guest=0x{:016X} {}x{}",
                  branch, impl_->guest_target_width, impl_->guest_target_height,
                  uint64_t(impl_->guest_gpu_pixels),
                  uint64_t(reinterpret_cast<uintptr_t>(impl_->guest_pixels)),
                  impl_->guest_width, impl_->guest_height);
    }
  }
  if (has_target) {
    const uint32_t copy_width =
        impl_->guest_width < impl_->width ? impl_->guest_width : impl_->width;
    const uint32_t copy_height =
        impl_->guest_height < impl_->height ? impl_->guest_height : impl_->height;
    static bool logged_target_copy = false;
    if (!logged_target_copy) {
      logged_target_copy = true;
      REXLOG_INFO("[deko3d] presenting render target 0x{:X}x{} as {}x{}",
                  impl_->guest_target_width, impl_->guest_target_height, copy_width,
                  copy_height);
    }
    impl_->gpu_cmdbuf.clear();
    // Run-060: a rotating command slot, not the swapchain slot. With the fence
    // gone the present loop can reach 25-30 Hz, so reusing three slots (keyed by
    // swapchain image) risks rewriting a copy list the GPU is still reading -
    // run 059 died ~9 s in with no guest violation, which is what that looks
    // like. 16 slots at ~30 Hz retires a slot after ~0.5 s.
    impl_->present_cmd_slot = (impl_->present_cmd_slot + 1) % kPresentCmdSlots;
    impl_->gpu_cmdbuf.addMemory(impl_->gpu_cmd_mem, impl_->present_cmd_slot * kCmdSize, kCmdSize);
    impl_->gpu_cmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dk::Image src_image = impl_->guest_target_image;
    dk::ImageView src_view{src_image};
    dk::ImageView dst_view{impl_->framebuffers[slot]};
    const DkImageRect src_rect{0, 0, 0, copy_width, copy_height, 1};
    const DkImageRect dst_rect{0, 0, 0, copy_width, copy_height, 1};
    impl_->gpu_cmdbuf.copyImage(src_view, src_rect, dst_view, dst_rect, 0);
    queue.submitCommands(impl_->gpu_cmdbuf.finishList());
    rex::graphics::deko3d::NoteGpuSubmit();
    queue.signalFence(impl_->present_fence, true);
    impl_->present_fence_at = impl_->present_count;
    // Run-069 (F1): at present 30 and 150, read back this exact source image -
    // the decisive blank/content question, answered with pixels, not inference.
    const uint32_t upcoming = impl_->present_count + 1;
    if ((upcoming == 30 || upcoming == 150) && impl_->guest_readbacks_done < 2) {
      ++impl_->guest_readbacks_done;
      impl_->ReadBackGuestOutputLocked(queue, upcoming);
    }
  } else if (has_gpu_output) {
    const uint32_t copy_width =
        impl_->guest_width < impl_->width ? impl_->guest_width : impl_->width;
    const uint32_t copy_height =
        impl_->guest_height < impl_->height ? impl_->guest_height : impl_->height;
    static bool logged_gpu_copy = false;
    if (!logged_gpu_copy) {
      logged_gpu_copy = true;
      REXLOG_INFO("[deko3d] gpu present copy: src 0x{:016X} pitch {} {}x{}",
                  uint64_t(impl_->guest_gpu_pixels), impl_->guest_gpu_pitch, copy_width,
                  copy_height);
    }
    // The copy fence, not a queue-wide idle wait, gates command-list reuse:
    // queue.waitIdle also waits for the present, which deadlocks with a held
    // window buffer (the same failure SVR2011 fixed with three swapchain
    // images and a copy fence).
    impl_->gpu_cmdbuf.clear();
    // Run-060: a rotating command slot, not the swapchain slot. With the fence
    // gone the present loop can reach 25-30 Hz, so reusing three slots (keyed by
    // swapchain image) risks rewriting a copy list the GPU is still reading -
    // run 059 died ~9 s in with no guest violation, which is what that looks
    // like. 16 slots at ~30 Hz retires a slot after ~0.5 s.
    impl_->present_cmd_slot = (impl_->present_cmd_slot + 1) % kPresentCmdSlots;
    impl_->gpu_cmdbuf.addMemory(impl_->gpu_cmd_mem, impl_->present_cmd_slot * kCmdSize, kCmdSize);
    // deko3d has no image layouts; a full barrier orders the acquired
    // presentable image and the render-target writes before the 2D engine
    // reads, and invalidates the image cache (plume's copy path does the same).
    impl_->gpu_cmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dk::ImageView dst_view{impl_->framebuffers[slot]};
    DkCopyBuf source{};
    source.addr = impl_->guest_gpu_pixels;
    source.rowLength = impl_->guest_gpu_pitch;
    source.imageHeight = impl_->guest_gpu_pitch * copy_height;
    const DkImageRect rect{0, 0, 0, copy_width, copy_height, 1};
    impl_->gpu_cmdbuf.copyBufferToImage(source, dst_view, rect);
    queue.submitCommands(impl_->gpu_cmdbuf.finishList());
    rex::graphics::deko3d::NoteGpuSubmit();
    queue.signalFence(impl_->present_fence, true);
    impl_->present_fence_at = impl_->present_count;
    if (logged_gpu_copy) {
      // Only the first frame reaches this diagnostic (logged_gpu_copy is set
      // above); keep it so the log proves the copy list was submitted.
      static bool logged_submit = false;
      if (!logged_submit) {
        logged_submit = true;
        REXLOG_INFO("[deko3d] gpu present copy submitted");
      }
    }
  } else if (has_guest_output) {
    const uint32_t copy_width =
        impl_->guest_width < impl_->width ? impl_->guest_width : impl_->width;
    const uint32_t copy_height =
        impl_->guest_height < impl_->height ? impl_->guest_height : impl_->height;
    const uint32_t row_bytes = copy_width * 4;
    uint8_t* dst = static_cast<uint8_t*>(impl_->upload_mem.getCpuAddr());
    const uint8_t* src = impl_->guest_pixels;
    for (uint32_t y = 0; y < copy_height; ++y) {
      std::memcpy(dst + size_t(y) * impl_->width * 4, src + size_t(y) * impl_->guest_pitch,
                  row_bytes);
    }
    impl_->upload_mem.flushCpuCache(0, impl_->width * impl_->height * 4);
    queue.submitCommands(impl_->guest_copy_cmdlists[slot]);
  } else {
    queue.submitCommands(impl_->pattern_cmdlist);
  }

  {
    // One line per presented frame. `age` counts presents since the guest output
    // was last refreshed (0 = this frame carries a fresh guest image), so the log
    // answers "does the display follow the guest?" directly: a run of age=0 with
    // increasing guest_refresh means yes; a climbing age means the presenter is
    // re-presenting a stale image.
    ++impl_->present_count;
    const auto now = std::chrono::steady_clock::now();
    const int64_t since_ms =
        impl_->refresh_epoch ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - impl_->last_refresh)
                                   .count()
                             : -1;
    const uint32_t age = impl_->present_count - impl_->present_at_last_refresh;
    REXLOG_WARN("[present] #{} slot={} branch={} guest#{} age={} since_refresh={}ms {}x{}",
                impl_->present_count, slot, branch_log, impl_->refresh_epoch, age, since_ms,
                impl_->guest_width, impl_->guest_height);
    // Run-069 death-watch: the ~9.5 s below-guest death writes no crash block, so
    // a periodic line names the last stage (uploads done, present slot) and the
    // memory left; the log tail then says what was in flight when it died.
    if ((impl_->present_count % 32) == 0) {
      REXLOG_WARN("[death-watch] present #{}: texture uploads so far={}, present cmd slot={}",
                  impl_->present_count, rex::graphics::deko3d::TextureUploadCount(),
                  impl_->present_cmd_slot);
      impl_->device->LogMemory("death-watch");
    }
  }
  queue.presentImage(impl_->swapchain, slot);
  return PaintResult::kPresented;
}

}  // namespace rex::ui::deko3d
