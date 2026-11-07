/**
 * @file        graphics/deko3d/shared_memory.h
 * @brief       GPU mirror of the guest's 512 MB physical memory for deko3d.
 *
 * The Vulkan backend keeps the guest memory and a device-local buffer in sync
 * with uploads and render-target resolves. deko3d does the same with a single
 * 512 MB mem block: the CPU copies newly-written guest pages into the block,
 * and the GPU writes resolves directly into it. The presenter reads resolved
 * frames from this block without a CPU readback.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <deko3d.hpp>

#include <rex/graphics/shared_memory.h>

namespace rex::ui::deko3d {
class Deko3DDevice;
}

namespace rex::graphics::deko3d {

class Deko3DSharedMemory final : public SharedMemory {
 public:
  Deko3DSharedMemory(ui::deko3d::Deko3DDevice& device, memory::Memory& memory);
  ~Deko3DSharedMemory() override;

  Deko3DSharedMemory(const Deko3DSharedMemory&) = delete;
  Deko3DSharedMemory& operator=(const Deko3DSharedMemory&) = delete;

  bool Initialize();
  void Shutdown();

  DkMemBlock mem_block() const { return mem_block_; }
  DkGpuAddr gpu_address() const { return gpu_address_; }
  uint8_t* cpu_address() const { return cpu_address_; }

 protected:
  bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  ui::deko3d::Deko3DDevice& device_;
  dk::UniqueMemBlock mem_block_;
  DkGpuAddr gpu_address_ = 0;
  uint8_t* cpu_address_ = nullptr;
};

}  // namespace rex::graphics::deko3d
