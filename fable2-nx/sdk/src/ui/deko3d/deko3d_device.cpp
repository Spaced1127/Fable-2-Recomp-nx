/**
 * @file        ui/deko3d/deko3d_device.cpp
 * @brief       Shared deko3d device and graphics queue.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>

namespace rex::ui::deko3d {

std::unique_ptr<Deko3DDevice> Deko3DDevice::Create() {
  std::unique_ptr<Deko3DDevice> device(new Deko3DDevice());
  device->device_ = dk::DeviceMaker{}.create();
  if (!device->device_) {
    REXLOG_ERROR("[deko3d] device creation failed");
    return nullptr;
  }
  // The default per-warp scratch (2 KB) is far below what Xenia's translated
  // shaders need; a shader asking for more than the queue has fails at draw
  // time with no diagnostic. The DKSH header's requirement is checked against
  // this value when the shader is loaded.
  device->queue_ = dk::QueueMaker{device->device_}
                       .setFlags(DkQueueFlags_Graphics)
                       .setPerWarpScratchMemorySize(kPerWarpScratchSize)
                       .create();
  if (!device->queue_) {
    REXLOG_ERROR("[deko3d] graphics queue creation failed");
    return nullptr;
  }
  device->render_queue_ = dk::QueueMaker{device->device_}
                              .setFlags(DkQueueFlags_Graphics)
                              .setPerWarpScratchMemorySize(kPerWarpScratchSize)
                              .create();
  if (!device->render_queue_) {
    REXLOG_ERROR("[deko3d] render queue creation failed");
    return nullptr;
  }
  return device;
}

Deko3DDevice::~Deko3DDevice() {
  if (queue_) {
    queue_.waitIdle();
  }
  if (render_queue_) {
    render_queue_.waitIdle();
  }
}

void Deko3DDevice::WaitIdle() {
  {
    std::lock_guard lock(submit_mutex_);
    if (queue_) {
      queue_.waitIdle();
    }
  }
  {
    std::lock_guard lock(render_mutex_);
    if (render_queue_) {
      render_queue_.waitIdle();
    }
  }
}

}  // namespace rex::ui::deko3d
