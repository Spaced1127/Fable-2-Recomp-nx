/**
 * @file        ui/deko3d/deko3d_device.h
 * @brief       Shared deko3d device and graphics queue for the Switch backend.
 *
 * The presenter and the Xenos command processor use the same device and the
 * same queue, so render-target clears, draws and the swapchain copy are ordered
 * in one command stream. On this deko3d a second graphics queue never executes
 * its command lists (measured on hardware), and each queue reserves its own
 * per-warp scratch pool, so there is exactly one queue.
 *
 * This object is owned by Deko3DProvider and borrowed by the presenter and the
 * graphics system.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <deko3d.hpp>

namespace rex::ui::deko3d {

class Deko3DDevice {
 public:
  // Per-warp scratch memory the queue is created with. deko3d reserves this for
  // every warp the GPU can run at once (about 8 MiB at 64 KiB per warp), out of a
  // process memory pool this port already runs at the edge of (3281 of 3285 MiB
  // measured at the menu), so it is not raised on speculation.
  //
  // tools/deko3d/inspect_pack.py reads the requirement straight out of the pack,
  // and all 28 menu programs ask for 0 bytes. If a later dump contains a shader
  // that needs more, Deko3DShaderCodeArena refuses it and logs the exact figure,
  // and this constant is where it gets raised. The value below is deko3d's own
  // default, 4 * DK_PER_WARP_SCRATCH_MEM_ALIGNMENT.
  static constexpr uint32_t kPerWarpScratchSize = 4 * DK_PER_WARP_SCRATCH_MEM_ALIGNMENT;

  static std::unique_ptr<Deko3DDevice> Create();
  ~Deko3DDevice();

  Deko3DDevice(const Deko3DDevice&) = delete;
  Deko3DDevice& operator=(const Deko3DDevice&) = delete;

  bool valid() const { return static_cast<bool>(device_) && static_cast<bool>(queue_); }

  dk::Device device() const { return device_; }
  dk::Queue queue() const { return queue_; }

  // Held while anything submits or presents.
  std::mutex& submit_mutex() { return submit_mutex_; }

  // Waits for all submitted work.
  void WaitIdle();

  // Logs the remaining heap and process memory pool, and flushes the log. The
  // Switch port runs close to its memory pool limit, so deko3d allocations are
  // attributed at startup and when one fails.
  void LogMemory(const char* where) const;

 private:
  Deko3DDevice() = default;

  dk::UniqueDevice device_;
  dk::UniqueQueue queue_;
  std::mutex submit_mutex_;
};

}  // namespace rex::ui::deko3d
