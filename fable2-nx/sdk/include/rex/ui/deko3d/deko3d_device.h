/**
 * @file        ui/deko3d/deko3d_device.h
 * @brief       Shared deko3d device and graphics queue for the Switch backend.
 *
 * The presenter and the Xenos command processor must record and submit to the
 * same device and queue so render-target resolves, texture uploads and the
 * swapchain copy are ordered. This object is owned by Deko3DProvider and
 * borrowed by the presenter and the graphics system.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <mutex>

#include <deko3d.hpp>

namespace rex::ui::deko3d {

class Deko3DDevice {
 public:
  // Per-warp scratch memory every queue is created with. uam reports each
  // shader's requirement in its DKSH program header; a shader needing more
  // than this is rejected at load rather than failing silently at draw time.
  static constexpr uint32_t kPerWarpScratchSize = 64 << 10;

  static std::unique_ptr<Deko3DDevice> Create();
  ~Deko3DDevice();

  Deko3DDevice(const Deko3DDevice&) = delete;
  Deko3DDevice& operator=(const Deko3DDevice&) = delete;

  bool valid() const {
    return static_cast<bool>(device_) && static_cast<bool>(queue_) &&
           static_cast<bool>(render_queue_);
  }

  dk::Device device() const { return device_; }
  dk::Queue queue() const { return queue_; }
  dk::Queue render_queue() const { return render_queue_; }

  // Held while anything submits or presents on the presentation queue.
  std::mutex& submit_mutex() { return submit_mutex_; }
  // Held while the command processor submits on the render queue.
  std::mutex& render_mutex() { return render_mutex_; }

  // Waits for all submitted work on both queues.
  void WaitIdle();

 private:
  Deko3DDevice() = default;

  dk::UniqueDevice device_;
  dk::UniqueQueue queue_;
  dk::UniqueQueue render_queue_;
  std::mutex submit_mutex_;
  std::mutex render_mutex_;
};

}  // namespace rex::ui::deko3d
