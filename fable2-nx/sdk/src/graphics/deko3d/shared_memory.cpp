/**
 * @file        graphics/deko3d/shared_memory.cpp
 * @brief       GPU mirror of the guest's 512 MB physical memory.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstring>

#include <rex/graphics/deko3d/shared_memory.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/deko3d/deko3d_device.h>

namespace rex::graphics::deko3d {

Deko3DSharedMemory::Deko3DSharedMemory(ui::deko3d::Deko3DDevice& device,
                                       memory::Memory& memory)
    : SharedMemory(memory), device_(device) {}

Deko3DSharedMemory::~Deko3DSharedMemory() {
  Shutdown();
}

bool Deko3DSharedMemory::Initialize() {
  InitializeCommon();

  // One full-size allocation, like the Vulkan backend's non-sparse fallback on
  // devices without sparse binding (including NVK on Tegra). The guest physical
  // address space is 512 MB, so offsets map directly.
  mem_block_ = dk::MemBlockMaker{device_.device(), kBufferSize}
                   .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
                   .create();
  if (!mem_block_) {
    REXGPU_ERROR("Shared memory: failed to allocate the {} MB deko3d block",
                 kBufferSize >> 20);
    return false;
  }
  cpu_address_ = static_cast<uint8_t*>(mem_block_.getCpuAddr());
  gpu_address_ = mem_block_.getGpuAddr();
  if (!cpu_address_ || !gpu_address_) {
    REXGPU_ERROR("Shared memory: deko3d block has no CPU/GPU address");
    mem_block_ = nullptr;
    return false;
  }
  // Starting from a defined state matters before the first resolve; the block
  // is also what the presenter shows until the game writes the front buffer.
  std::memset(cpu_address_, 0, kBufferSize);
  mem_block_.flushCpuCache(0, kBufferSize);
  REXGPU_INFO("Shared memory: {} MB deko3d block at GPU 0x{:016X}", kBufferSize >> 20,
              uint64_t(gpu_address_));
  return true;
}

void Deko3DSharedMemory::Shutdown() {
  ShutdownCommon();
  mem_block_ = nullptr;
  cpu_address_ = nullptr;
  gpu_address_ = 0;
}

bool Deko3DSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  for (const std::pair<uint32_t, uint32_t>& upload_range : upload_page_ranges) {
    const uint32_t start = upload_range.first << page_size_log2();
    const uint32_t length = upload_range.second << page_size_log2();
    if (!length || start >= kBufferSize) {
      continue;
    }
    const uint32_t clamped_length = std::min(length, kBufferSize - start);

    // Mark valid before copying, as the common layer requires: a CPU write
    // racing with the copy must not be missed.
    MakeRangeValid(start, clamped_length, false);
    const uint8_t* source = memory().TranslatePhysical(start);
    if (!source) {
      REXGPU_ERROR("Shared memory: no guest mapping for upload at 0x{:08X}", start);
      return false;
    }
    std::memcpy(cpu_address_ + start, source, clamped_length);
    if (mem_block_.flushCpuCache(start, clamped_length) != DkResult_Success) {
      REXGPU_ERROR("Shared memory: flush of 0x{:08X}+0x{:X} failed", start,
                   clamped_length);
      return false;
    }
  }
  return true;
}

}  // namespace rex::graphics::deko3d
