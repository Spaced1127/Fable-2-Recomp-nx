/**
 * @file        ui/deko3d/deko3d_provider.cpp
 * @brief       GraphicsProvider for the deko3d backend
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/deko3d/deko3d_presenter.h>
#include <rex/ui/deko3d/deko3d_provider.h>

namespace rex::ui::deko3d {

Deko3DProvider::Deko3DProvider() : device_(Deko3DDevice::Create()) {}
Deko3DProvider::~Deko3DProvider() = default;

std::unique_ptr<Presenter> Deko3DProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return std::make_unique<Deko3DPresenter>(device_.get(),
                                           std::move(host_gpu_loss_callback));
}

std::unique_ptr<ImmediateDrawer> Deko3DProvider::CreateImmediateDrawer() {
  // No deko3d implementation of the UI immediate drawer yet; the overlays that
  // need one stay disabled on this backend.
  return nullptr;
}

}  // namespace rex::ui::deko3d
