/**
 * @file        ui/deko3d/deko3d_provider.h
 * @brief       GraphicsProvider for the deko3d backend (Nintendo Switch)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/ui/deko3d/deko3d_device.h>
#include <rex/ui/graphics_provider.h>

namespace rex::ui::deko3d {

class Deko3DProvider final : public GraphicsProvider {
 public:
  Deko3DProvider();
  ~Deko3DProvider() override;

  // Shared device/queue used by both the presenter and the Xenos command
  // processor. May be null if deko3d initialization failed.
  Deko3DDevice* device() const { return device_.get(); }

  std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback host_gpu_loss_callback) override;
  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

 private:
  std::unique_ptr<Deko3DDevice> device_;
};

}  // namespace rex::ui::deko3d
