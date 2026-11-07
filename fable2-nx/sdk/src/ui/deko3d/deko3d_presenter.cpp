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
#include <cstring>
#include <mutex>

#include <switch.h>
#include <deko3d.hpp>

#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>
#include <rex/ui/deko3d/deko3d_presenter.h>
#include <rex/ui/surface_switch.h>

namespace rex::ui::deko3d {

namespace {

constexpr unsigned kNumFramebuffers = 3;
constexpr uint32_t kCmdSize = 0x1000;

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
  dk::Fence copy_fence{};
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
    copy_pending = false;
    width = height = 0;
    guest_pixels = nullptr;
    guest_width = guest_height = guest_pitch = 0;
    guest_gpu_pixels = 0;
    guest_gpu_pitch = 0;
    guest_target_image = dk::Image{};
    guest_target_width = guest_target_height = 0;
    has_guest_target = false;
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
    gpu_cmd_mem = dk::MemBlockMaker{dk_device, kCmdSize * kNumFramebuffers}
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
  // The previous frame's copy must be finished before its command slot is
  // reused. The fence signs after the copy and before the present, so a paused
  // display (HOME, capture) does not hold this wait.
  if (impl_->copy_pending) {
    const DkResult copy_wait = impl_->copy_fence.wait(2000000000);
    if (copy_wait == DkResult_Timeout) {
      REXLOG_ERROR("[deko3d] present copy fence timed out (queue error state {})",
                   impl_->device->queue().isInErrorState() ? 1 : 0);
      impl_->copy_pending = false;
      return PaintResult::kNotPresented;
    }
    impl_->copy_pending = false;
  }
  std::lock_guard dev_lock(impl_->device->submit_mutex());
  dk::Queue queue = impl_->device->queue();
  const int slot = queue.acquireImage(impl_->swapchain);
  queue.submitCommands(impl_->bind_cmdlists[slot]);

  const bool has_target = impl_->has_guest_target && impl_->guest_target_width &&
                          impl_->guest_target_height && impl_->guest_width &&
                          impl_->guest_height;
  const bool has_gpu_output = impl_->guest_gpu_pixels && impl_->guest_width &&
                              impl_->guest_height && impl_->guest_gpu_pitch;
  const bool has_guest_output = impl_->guest_pixels && impl_->guest_width &&
                                impl_->guest_height && impl_->guest_pitch;
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
    impl_->gpu_cmdbuf.addMemory(impl_->gpu_cmd_mem, slot * kCmdSize, kCmdSize);
    impl_->gpu_cmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dk::Image src_image = impl_->guest_target_image;
    dk::ImageView src_view{src_image};
    dk::ImageView dst_view{impl_->framebuffers[slot]};
    const DkImageRect src_rect{0, 0, 0, copy_width, copy_height, 1};
    const DkImageRect dst_rect{0, 0, 0, copy_width, copy_height, 1};
    impl_->gpu_cmdbuf.copyImage(src_view, src_rect, dst_view, dst_rect, 0);
    queue.submitCommands(impl_->gpu_cmdbuf.finishList());
    queue.signalFence(impl_->copy_fence, true);
    impl_->copy_pending = true;
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
    impl_->gpu_cmdbuf.addMemory(impl_->gpu_cmd_mem, slot * kCmdSize, kCmdSize);
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
    queue.signalFence(impl_->copy_fence, true);
    impl_->copy_pending = true;
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

  queue.presentImage(impl_->swapchain, slot);
  return PaintResult::kPresented;
}

}  // namespace rex::ui::deko3d
