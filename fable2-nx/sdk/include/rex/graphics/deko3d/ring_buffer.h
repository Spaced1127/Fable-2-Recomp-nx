/**
 * @file        graphics/deko3d/ring_buffer.h
 * @brief       Bump allocator over one GPU-visible deko3d memory block.
 *
 * Used for the per-draw constant buffers (system, float, bool/loop, fetch),
 * the per-frame converted index buffers and the per-draw image/sampler
 * descriptor sets. deko3d has no dynamic-offset descriptor indirection, so
 * every draw gets its own slice of the block and binds it by GPU address.
 *
 * The block is CpuCached: writes are collected in the CPU cache and pushed
 * with one Flush() before the command list that reads them is submitted.
 * Reset() may only be called once the GPU has finished with the previous
 * contents - the command processor does that at the frame boundary, where the
 * render target cache has already waited on the previous frame's fence.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <deko3d.hpp>

namespace rex::ui::deko3d {
class Deko3DDevice;
}

namespace rex::graphics::deko3d {

class Deko3DGpuRing {
 public:
  Deko3DGpuRing(ui::deko3d::Deko3DDevice& device, uint32_t size_bytes, const char* name);
  ~Deko3DGpuRing();

  Deko3DGpuRing(const Deko3DGpuRing&) = delete;
  Deko3DGpuRing& operator=(const Deko3DGpuRing&) = delete;

  bool Initialize();
  void Shutdown();

  bool valid() const { return static_cast<bool>(memory_); }

  // Bump-allocates size_bytes at the given alignment. Returns nullptr when the
  // ring is full; the caller must then finish and submit the current command
  // list, wait for it, and Reset().
  void* Request(uint32_t size_bytes, uint32_t alignment, DkGpuAddr& gpu_address_out);

  // Pushes everything written since the previous Flush() (or Reset()) out of
  // the CPU cache. Must be called before submitting commands that read it.
  void Flush();

  void Reset();

  DkGpuAddr gpu_address() const { return gpu_address_; }
  uint32_t size() const { return size_bytes_; }
  uint32_t used() const { return offset_; }
  const char* name() const { return name_; }

 private:
  ui::deko3d::Deko3DDevice& device_;
  uint32_t size_bytes_;
  const char* name_;
  dk::UniqueMemBlock memory_;
  uint8_t* cpu_address_ = nullptr;
  DkGpuAddr gpu_address_ = 0;
  uint32_t offset_ = 0;
  uint32_t flush_start_ = 0;
};

// Pushes a byte range of a block out of the CPU cache. deko3d's cache
// maintenance works on whole cache lines, so the range is widened to them and
// clamped to the block; false means deko3d refused it.
bool FlushCpuRange(dk::MemBlock block, uint32_t offset, uint32_t size);

}  // namespace rex::graphics::deko3d
