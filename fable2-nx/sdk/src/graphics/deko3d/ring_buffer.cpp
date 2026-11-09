/**
 * @file        graphics/deko3d/ring_buffer.cpp
 * @brief       Bump allocator over one GPU-visible deko3d memory block.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>

#include <rex/graphics/deko3d/ring_buffer.h>
#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>

namespace rex::graphics::deko3d {

namespace {

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

Deko3DGpuRing::Deko3DGpuRing(ui::deko3d::Deko3DDevice& device, uint32_t size_bytes,
                             const char* name)
    : device_(device), size_bytes_(size_bytes), name_(name) {}

Deko3DGpuRing::~Deko3DGpuRing() {
  Shutdown();
}

bool Deko3DGpuRing::Initialize() {
  // deko3d memory blocks are whole pages.
  const uint32_t block_size = AlignUp(std::max(size_bytes_, uint32_t(DK_MEMBLOCK_ALIGNMENT)),
                                      DK_MEMBLOCK_ALIGNMENT);
  memory_ = dk::MemBlockMaker{device_.device(), block_size}
                .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
                .create();
  if (!memory_) {
    REXGPU_ERROR("deko3d: {} ring allocation failed ({} bytes)", name_, block_size);
    device_.LogMemory("ring allocation failed");
    return false;
  }
  cpu_address_ = static_cast<uint8_t*>(memory_.getCpuAddr());
  gpu_address_ = memory_.getGpuAddr();
  size_bytes_ = block_size;
  offset_ = 0;
  flush_start_ = 0;
  return true;
}

void Deko3DGpuRing::Shutdown() {
  memory_ = nullptr;
  cpu_address_ = nullptr;
  gpu_address_ = 0;
  offset_ = 0;
  flush_start_ = 0;
}

void* Deko3DGpuRing::Request(uint32_t size_bytes, uint32_t alignment,
                             DkGpuAddr& gpu_address_out) {
  gpu_address_out = 0;
  if (!memory_ || !size_bytes) {
    return nullptr;
  }
  const uint32_t aligned_offset = AlignUp(offset_, std::max(alignment, uint32_t(1)));
  if (uint64_t(aligned_offset) + size_bytes > size_bytes_) {
    return nullptr;
  }
  offset_ = aligned_offset + size_bytes;
  gpu_address_out = gpu_address_ + aligned_offset;
  return cpu_address_ + aligned_offset;
}

void Deko3DGpuRing::Flush() {
  if (!memory_ || offset_ <= flush_start_) {
    return;
  }
  memory_.flushCpuCache(flush_start_, offset_ - flush_start_);
  flush_start_ = offset_;
}

void Deko3DGpuRing::Reset() {
  offset_ = 0;
  flush_start_ = 0;
}

}  // namespace rex::graphics::deko3d
