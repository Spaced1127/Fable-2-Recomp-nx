/**
 * @file        graphics/deko3d/primitive_processor.h
 * @brief       Primitive processing for the deko3d backend.
 *
 * The guest's primitive types are converted the same way the Vulkan backend
 * converts them (triangle fans to lists, line loops to strips, points and
 * rectangle lists expanded in the vertex shader), so the host vertex shader
 * type - and therefore the DKSH modification the shader pack is keyed by -
 * matches what the Vulkan dump produced.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include <deko3d.hpp>

#include <rex/graphics/deko3d/ring_buffer.h>
#include <rex/graphics/primitive_processor.h>

namespace rex::ui::deko3d {
class Deko3DDevice;
}

namespace rex::graphics::deko3d {

class Deko3DPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  Deko3DPrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                           SharedMemory& shared_memory, ui::deko3d::Deko3DDevice& device);
  ~Deko3DPrimitiveProcessor() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  // Called when a new command list is opened: the previous one has been
  // submitted and its fence waited on, so the converted indices it read can be
  // overwritten.
  void ResetPerFrame();
  // Pushes this frame's converted indices out of the CPU cache. Must be called
  // before the command list that reads them is submitted.
  void FlushConvertedIndices() { index_ring_.Flush(); }

  // GPU address of the built-in index buffer (auto-index expansion and
  // primitive type conversion), at the handle's offset.
  DkGpuAddr GetBuiltinIndexBufferAddress(size_t handle) const {
    return builtin_address_ + GetBuiltinIndexBufferOffsetBytes(handle);
  }
  // GPU address of a converted index buffer requested during the current draw.
  DkGpuAddr GetConvertedIndexBufferAddress(size_t handle) const {
    return handle < frame_index_buffers_.size() ? frame_index_buffers_[handle] : DkGpuAddr(0);
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  ui::deko3d::Deko3DDevice& device_;
  Deko3DGpuRing index_ring_;
  // Indexed by the backend handles handed out this frame.
  std::vector<DkGpuAddr> frame_index_buffers_;
  // Session-long; the built-in index buffer never changes after initialization.
  dk::UniqueMemBlock builtin_memory_;
  DkGpuAddr builtin_address_ = 0;
  bool shutdown_ = false;
};

}  // namespace rex::graphics::deko3d
