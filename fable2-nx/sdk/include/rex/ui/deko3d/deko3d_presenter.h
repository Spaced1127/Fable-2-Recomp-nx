/**
 * @file        ui/deko3d/deko3d_presenter.h
 * @brief       Presenter for the deko3d backend: presents on the libnx NWindow
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>

#include <deko3d.hpp>

#include <rex/ui/presenter.h>

namespace rex::ui::deko3d {

class Deko3DDevice;

class Deko3DPresenter final : public Presenter {
 public:
  Deko3DPresenter(Deko3DDevice* device, HostGpuLossCallback host_gpu_loss_callback);
  ~Deko3DPresenter() override;

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(RawImage& image_out) override;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(
      uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
      std::function<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out_ref) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The swap path hands the guest front buffer's host address to the presenter
// through this context. The layout is a linear 8_8_8_8 image, pitch =
// width * 4 (the guest's resolved front buffer).
class Deko3DGuestOutputRefreshContext final : public Presenter::GuestOutputRefreshContext {
 public:
  explicit Deko3DGuestOutputRefreshContext(bool& is_8bpc_out_ref)
      : GuestOutputRefreshContext(is_8bpc_out_ref) {}

  void SetGuestOutput(const uint8_t* pixels, uint32_t width, uint32_t height,
                      uint32_t pitch) {
    pixels_ = pixels;
    width_ = width;
    height_ = height;
    pitch_ = pitch;
    gpu_pixels_ = 0;
    gpu_pitch_ = 0;
    target_image_ = dk::Image{};
    has_target_ = false;
  }

  // The resolved front buffer in the shared-memory GPU mirror. When nonzero,
  // the presenter copies it directly instead of reading guest memory.
  void SetGuestOutputGpu(DkGpuAddr pixels, uint32_t width, uint32_t height,
                         uint32_t pitch) {
    pixels_ = nullptr;
    width_ = width;
    height_ = height;
    pitch_ = pitch;
    gpu_pixels_ = pixels;
    gpu_pitch_ = pitch;
    target_image_ = dk::Image{};
    has_target_ = false;
  }

  // A host render target to present directly (D2: the guest-buffer resolve is
  // deferred until the DKSH compute resolve exists).
  void SetGuestOutputTarget(const dk::Image& image, uint32_t image_width, uint32_t image_height,
                            uint32_t view_width, uint32_t view_height) {
    pixels_ = nullptr;
    gpu_pixels_ = 0;
    width_ = view_width;
    height_ = view_height;
    pitch_ = view_width * 4;
    target_image_ = image;
    target_width_ = image_width;
    target_height_ = image_height;
    has_target_ = true;
  }

  const uint8_t* pixels() const { return pixels_; }
  DkGpuAddr gpu_pixels() const { return gpu_pixels_; }
  dk::Image target_image() const { return target_image_; }
  uint32_t target_width() const { return target_width_; }
  uint32_t target_height() const { return target_height_; }
  bool has_target() const { return has_target_; }
  void SetRenderFence(DkFence* fence) { render_fence_ = fence; }
  DkFence* render_fence() const { return render_fence_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t pitch() const { return pitch_; }
  uint32_t gpu_pitch() const { return gpu_pitch_; }

 private:
  const uint8_t* pixels_ = nullptr;
  DkGpuAddr gpu_pixels_ = 0;
  dk::Image target_image_{};
  uint32_t target_width_ = 0;
  uint32_t target_height_ = 0;
  bool has_target_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t pitch_ = 0;
  uint32_t gpu_pitch_ = 0;
  DkFence* render_fence_ = nullptr;
};

}  // namespace rex::ui::deko3d
