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
// Room for the converted indices of a frame's draws. A single draw can need
// more than this (a maximum-size triangle fan becomes ~12 MB of indices), and
// then it is skipped with a reason instead of the ring being sized for the
// worst case all the time.
constexpr uint32_t kConvertedIndexRingSize = 4 << 20;

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
  REXGPU_WARN("deko3d: primitive processor: converting fans, loops and quads");
  // deko3d takes full 32-bit indices (DkIdxFormat_Uint32), so index indirection
  // in the vertex shader is never needed for range.
  //
  // Triangle fans, line loops and quad lists are converted with the built-in
  // index buffer rather than passed to the host, which keeps the host vertex
  // shader type at kVertex - the only variant in the DKSH pack (the dump has no
  // other host_vertex_shader_type, and the Vulkan backend converts fans for
  // D3D12 parity, so this also matches the shaders that were captured).
  //
  // Point sprites and rectangle lists are left alone instead of expanded in the
  // vertex shader, for two reasons: expanding them needs a translated vertex
  // shader variant the pack does not contain, and it is what makes the common
  // code allocate a two-triangle-strip built-in index buffer of about 1.5 MiB
  // (rectangle lists) on top of ~2 MiB more (point sprites) - memory this port
  // does not have to spare. deko3d draws points natively; rectangle lists have
  // no deko3d primitive and are skipped with a reason.
  if (!InitializeCommon(true, false, false, false, true, true)) {
    REXGPU_WARN("deko3d: primitive processor: InitializeCommon failed");
    Shutdown();
    return false;
  }
  REXGPU_WARN("deko3d: primitive processor: converted index ring {} KiB",
              kConvertedIndexRingSize >> 10);
  if (!index_ring_.Initialize()) {
    REXGPU_WARN("deko3d: primitive processor: converted index ring allocation failed");
    Shutdown();
    return false;
  }
  REXGPU_WARN("deko3d: primitive processor ready");
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
  // A warning: these are the lines that have to survive if the process dies
  // here, which is exactly what this step is being used to find out.
  REXGPU_WARN("deko3d: built-in index buffer: allocating {} KiB", uint32_t(size_bytes) >> 10);
  const uint32_t block_size =
      AlignUp(uint32_t(size_bytes), uint32_t(DK_MEMBLOCK_ALIGNMENT));
  builtin_memory_ = dk::MemBlockMaker{device_.device(), block_size}
                        .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
                        .create();
  if (!builtin_memory_) {
    REXGPU_WARN("deko3d: built-in index buffer allocation failed ({} bytes)", size_bytes);
    device_.LogMemory("built-in index buffer allocation failed");
    return false;
  }
  void* mapping = builtin_memory_.getCpuAddr();
  if (!mapping) {
    REXGPU_WARN("deko3d: built-in index buffer has no CPU address");
    builtin_memory_ = nullptr;
    return false;
  }
  REXGPU_WARN("deko3d: built-in index buffer: filling at {}", reinterpret_cast<uintptr_t>(mapping));
  fill_callback(mapping);
  REXGPU_WARN("deko3d: built-in index buffer: filled, flushing {} bytes", size_bytes);
  if (!FlushCpuRange(builtin_memory_, 0, uint32_t(size_bytes))) {
    REXGPU_WARN("deko3d: built-in index buffer cache flush refused");
  }
  builtin_address_ = builtin_memory_.getGpuAddr();
  REXGPU_WARN("deko3d: built-in index buffer ready at GPU 0x{:016X}", uint64_t(builtin_address_));
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
