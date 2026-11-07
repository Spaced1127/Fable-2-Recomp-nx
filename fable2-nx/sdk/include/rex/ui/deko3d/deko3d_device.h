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
  static std::unique_ptr<Deko3DDevice> Create();
  ~Deko3DDevice();

  Deko3DDevice(const Deko3DDevice&) = delete;
  Deko3DDevice& operator=(const Deko3DDevice&) = delete;

  bool valid() const { return static_cast<bool>(device_) && static_cast<bool>(queue_); }

  dk::Device device() const { return device_; }
  dk::Queue queue() const { return queue_; }

  // Held while anything submits or presents on the queue. Callers must not
  // call WaitIdle while holding it.
  std::mutex& submit_mutex() { return submit_mutex_; }

  // Waits for all submitted work. Takes the submit mutex internally.
  void WaitIdle();

 private:
  Deko3DDevice() = default;

  dk::UniqueDevice device_;
  dk::UniqueQueue queue_;
  std::mutex submit_mutex_;
};

}  // namespace rex::ui::deko3d
