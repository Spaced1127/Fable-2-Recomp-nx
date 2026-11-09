/**
 * @file        graphics/deko3d/primitive_processor.cpp
 * @brief       Primitive processing for the deko3d backend.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstring>

#include <rex/graphics/deko3d/primitive_processor.h>
#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>

namespace rex::graphics::deko3d {

namespace {
// Room for the converted indices of a frame's draws. One draw can need up to
// kMinRequiredConvertedIndexBufferSize on its own (triangle fan to list
// conversion), so this is a few draws' worth of headroom on top.
constexpr uint32_t kConvertedIndexRingSize = 8 << 20;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
}  // namespace

Deko3DPrimitiveProcessor::Deko3DPrimitiveProcessor(const RegisterFile& register_file,
                                                   memory::Memory& memory,
                                                   SharedMemory& shared_memory,
                                                   ui::deko3d::Deko3DDevice& device)
    : PrimitiveProcessor(register_file, memory, shared_memory),
      index_ring_(device, kConvertedIndexRingSize, "converted index") ,
      device_(device) {}

Deko3DPrimitiveProcessor::~Deko3DPrimitiveProcessor() {
  Shutdown(true);
}

bool Deko3DPrimitiveProcessor::Initialize() {
  // deko3d takes full 32-bit indices (DkIdxFormat_UInt32), so no indirection is
  // ever needed in the vertex shader.
  //
  // Everything else is converted rather than passed to the host, matching the
  // Vulkan backend's settings on a device without geometry shaders. That is
  // what the DKSH pack was generated with: converting keeps the host vertex
  // shader type at kVertex for ordinary draws, so the shader modification -
  // part of the pack key - matches the dumped one. Point, rectangle list and
  // quad list draws need their own translated vertex shaders and are reported
  // as missing from the pack until a dump includes them.
  if (!InitializeCommon(true, false, false, false, false, false)) {
    Shutdown();
    return false;
  }
  if (!index_ring_.Initialize()) {
    Shutdown();
    return false;
  }
  return true;
}

void Deko3DPrimitiveProcessor::Shutdown(bool from_destructor) {
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  frame_index_buffers_.clear();
  index_ring_.Shutdown();
  builtin_memory_ = nullptr;
  builtin_address_ = 0;
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void Deko3DPrimitiveProcessor::ResetPerFrame() {
  frame_index_buffers_.clear();
  index_ring_.Reset();
}

bool Deko3DPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  if (!size_bytes || builtin_memory_) {
    return false;
  }
  const uint32_t block_size =
      AlignUp(uint32_t(size_bytes), uint32_t(DK_MEMBLOCK_ALIGNMENT));
  builtin_memory_ = dk::MemBlockMaker{device_.device(), block_size}
                        .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
                        .create();
  if (!builtin_memory_) {
    REXGPU_ERROR("deko3d: built-in index buffer allocation failed ({} bytes)", size_bytes);
    return false;
  }
  void* mapping = builtin_memory_.getCpuAddr();
  fill_callback(mapping);
  builtin_memory_.flushCpuCache(0, uint32_t(size_bytes));
  builtin_address_ = builtin_memory_.getGpuAddr();
  return true;
}

void* Deko3DPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  const uint32_t index_size =
      format == xenos::IndexFormat::kInt16 ? uint32_t(sizeof(uint16_t)) : uint32_t(sizeof(uint32_t));
  const uint32_t size_bytes =
      index_size * index_count + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0);
  DkGpuAddr gpu_address = 0;
  uint8_t* mapping =
      static_cast<uint8_t*>(index_ring_.Request(size_bytes, index_size, gpu_address));
  if (!mapping) {
    return nullptr;
  }
  if (coalign_for_simd) {
    // The allocation has XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE bytes of excess
    // for this; move forward only, into the padding.
    const ptrdiff_t coalignment_offset =
        GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    gpu_address += DkGpuAddr(coalignment_offset);
  }
  backend_handle_out = frame_index_buffers_.size();
  frame_index_buffers_.push_back(gpu_address);
  return mapping;
}

}  // namespace rex::graphics::deko3d
