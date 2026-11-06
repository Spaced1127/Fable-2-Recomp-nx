/**
 * @file        ui/deko3d/deko3d_provider.h
 * @brief       GraphicsProvider for the deko3d backend (Nintendo Switch)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/ui/graphics_provider.h>

namespace rex::ui::deko3d {

class Deko3DProvider final : public GraphicsProvider {
 public:
  Deko3DProvider();
  ~Deko3DProvider() override;

  std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback host_gpu_loss_callback) override;
  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;
};

}  // namespace rex::ui::deko3d
