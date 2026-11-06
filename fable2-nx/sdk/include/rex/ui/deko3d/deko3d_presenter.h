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

#include <rex/ui/presenter.h>

namespace rex::ui::deko3d {

class Deko3DPresenter final : public Presenter {
 public:
  explicit Deko3DPresenter(HostGpuLossCallback host_gpu_loss_callback);
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
  }

  const uint8_t* pixels() const { return pixels_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t pitch() const { return pitch_; }

 private:
  const uint8_t* pixels_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t pitch_ = 0;
};

}  // namespace rex::ui::deko3d
