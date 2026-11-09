/**
 * @file        graphics/deko3d/command_processor.cpp
 * @brief       CommandProcessor for the deko3d backend: guest ring submission,
 *              draws and presentation.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <xxhash.h>

#include <rex/graphics/deko3d/command_processor.h>
#include <rex/graphics/deko3d/graphics_system.h>
#include <rex/graphics/deko3d/primitive_processor.h>
#include <rex/graphics/deko3d/render_target_cache.h>
#include <rex/graphics/deko3d/shader.h>
#include <rex/graphics/deko3d/shader_pack.h>
#include <rex/graphics/deko3d/shared_memory.h>
#include <rex/graphics/deko3d/texture_cache.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/deko3d/deko3d_device.h>
#include <rex/ui/deko3d/deko3d_presenter.h>
#include <rex/ui/presenter.h>

#if REX_PLATFORM_SWITCH
// The profiler's guest frame counter (switch_perf.cpp).
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

namespace rex::graphics::deko3d {

namespace {

// Per-frame ring sizes. Both are recycled when the render target cache opens a
// new command list, which is the one point where the previous list is known to
// have completed. Kept deliberately small: the process runs close to its
// Horizon memory pool limit (the guest mirror alone is 512 MB), and a request
// that does not fit skips the draw with a logged reason rather than costing
// tens of megabytes that may not be there.
constexpr uint32_t kConstantRingSize = 4 << 20;
constexpr uint32_t kDescriptorRingSize = 1 << 20;

// deko3d image and sampler descriptors are 32 bytes each.
constexpr uint32_t kDescriptorSize = 32;
// Alignment each descriptor set's base address gets in the ring.
constexpr uint32_t kDescriptorSetAlignment = 256;
// Uniform and storage buffer offsets must be page-fraction aligned; deko3d's
// own alignment for uniform buffers is the safe choice for both.
constexpr uint32_t kBufferAlignment = DK_UNIFORM_BUF_ALIGNMENT;

// Binding slots the converted GLSL declares (see docs/DEKO3D_BACKEND.md).
constexpr uint32_t kStorageBufferSystemConstants = 0;
constexpr uint32_t kStorageBufferSharedMemory = 1;
constexpr uint32_t kUniformBufferFloatVertex = 1;
constexpr uint32_t kUniformBufferFloatPixel = 2;
constexpr uint32_t kUniformBufferBoolLoop = 3;
constexpr uint32_t kUniformBufferFetch = 4;

constexpr size_t kBoolLoopConstantsSize = sizeof(uint32_t) * (8 + 32);
constexpr size_t kFetchConstantsSize = sizeof(uint32_t) * 6 * 32;

// The most texture bindings a single stage can declare (deko3d has 32 texture
// slots per stage, and Xenos has 32 texture fetch constants).
constexpr uint32_t kMaxTextureBindingsPerStage = 32;

DkPrimitive GetDkPrimitive(xenos::PrimitiveType type, bool& supported_out) {
  supported_out = true;
  switch (type) {
    case xenos::PrimitiveType::kPointList:
      return DkPrimitive_Points;
    case xenos::PrimitiveType::kLineList:
      return DkPrimitive_Lines;
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kTriangleWithWFlags:
      return DkPrimitive_Triangles;
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::k2DTriStrip:
      return DkPrimitive_TriangleStrip;
    case xenos::PrimitiveType::kTriangleFan:
      return DkPrimitive_TriangleFan;
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::k2DLineStrip:
      return DkPrimitive_LineStrip;
    case xenos::PrimitiveType::kLineLoop:
      return DkPrimitive_LineLoop;
    case xenos::PrimitiveType::kQuadList:
      return DkPrimitive_Quads;
    case xenos::PrimitiveType::kQuadStrip:
      return DkPrimitive_QuadStrip;
    case xenos::PrimitiveType::kPolygon:
      return DkPrimitive_Polygon;
    default:
      supported_out = false;
      return DkPrimitive_Triangles;
  }
}

DkCompareOp GetDkCompareOp(xenos::CompareFunction function) {
  switch (function) {
    case xenos::CompareFunction::kNever:
      return DkCompareOp_Never;
    case xenos::CompareFunction::kLess:
      return DkCompareOp_Less;
    case xenos::CompareFunction::kEqual:
      return DkCompareOp_Equal;
    case xenos::CompareFunction::kLessEqual:
      return DkCompareOp_Lequal;
    case xenos::CompareFunction::kGreater:
      return DkCompareOp_Greater;
    case xenos::CompareFunction::kNotEqual:
      return DkCompareOp_NotEqual;
    case xenos::CompareFunction::kGreaterEqual:
      return DkCompareOp_Gequal;
    default:
      return DkCompareOp_Always;
  }
}

DkStencilOp GetDkStencilOp(xenos::StencilOp op) {
  switch (op) {
    case xenos::StencilOp::kZero:
      return DkStencilOp_Zero;
    case xenos::StencilOp::kReplace:
      return DkStencilOp_Replace;
    case xenos::StencilOp::kIncrementClamp:
      return DkStencilOp_Incr;
    case xenos::StencilOp::kDecrementClamp:
      return DkStencilOp_Decr;
    case xenos::StencilOp::kInvert:
      return DkStencilOp_Invert;
    case xenos::StencilOp::kIncrementWrap:
      return DkStencilOp_IncrWrap;
    case xenos::StencilOp::kDecrementWrap:
      return DkStencilOp_DecrWrap;
    default:
      return DkStencilOp_Keep;
  }
}

DkBlendOp GetDkBlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract:
      return DkBlendOp_Sub;
    case xenos::BlendOp::kRevSubtract:
      return DkBlendOp_RevSub;
    case xenos::BlendOp::kMin:
      return DkBlendOp_Min;
    case xenos::BlendOp::kMax:
      return DkBlendOp_Max;
    default:
      return DkBlendOp_Add;
  }
}

DkBlendFactor GetDkBlendFactor(xenos::BlendFactor factor) {
  switch (factor) {
    case xenos::BlendFactor::kZero:
      return DkBlendFactor_Zero;
    case xenos::BlendFactor::kOne:
      return DkBlendFactor_One;
    case xenos::BlendFactor::kSrcColor:
      return DkBlendFactor_SrcColor;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return DkBlendFactor_InvSrcColor;
    case xenos::BlendFactor::kSrcAlpha:
      return DkBlendFactor_SrcAlpha;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return DkBlendFactor_InvSrcAlpha;
    case xenos::BlendFactor::kDstColor:
      return DkBlendFactor_DstColor;
    case xenos::BlendFactor::kOneMinusDstColor:
      return DkBlendFactor_InvDstColor;
    case xenos::BlendFactor::kDstAlpha:
      return DkBlendFactor_DstAlpha;
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return DkBlendFactor_InvDstAlpha;
    case xenos::BlendFactor::kConstantColor:
      return DkBlendFactor_ConstColor;
    case xenos::BlendFactor::kOneMinusConstantColor:
      return DkBlendFactor_InvConstColor;
    case xenos::BlendFactor::kConstantAlpha:
      return DkBlendFactor_ConstAlpha;
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return DkBlendFactor_InvConstAlpha;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return DkBlendFactor_SrcAlphaSaturate;
    default:
      return DkBlendFactor_One;
  }
}

// deko3d has every Xenos wrap mode natively, so unlike the Vulkan backend no
// clamp mode has to be approximated.
DkWrapMode GetDkWrapMode(xenos::ClampMode clamp_mode) {
  switch (clamp_mode) {
    case xenos::ClampMode::kRepeat:
      return DkWrapMode_Repeat;
    case xenos::ClampMode::kMirroredRepeat:
      return DkWrapMode_MirroredRepeat;
    case xenos::ClampMode::kClampToEdge:
      return DkWrapMode_ClampToEdge;
    case xenos::ClampMode::kMirrorClampToEdge:
      return DkWrapMode_MirrorClampToEdge;
    case xenos::ClampMode::kClampToHalfway:
      return DkWrapMode_Clamp;
    case xenos::ClampMode::kMirrorClampToHalfway:
      return DkWrapMode_MirrorClamp;
    case xenos::ClampMode::kClampToBorder:
      return DkWrapMode_ClampToBorder;
    default:
      return DkWrapMode_MirrorClampToBorder;
  }
}

DkMsMode GetMsMode(xenos::MsaaSamples samples) {
  switch (samples) {
    case xenos::MsaaSamples::k2X:
      return DkMsMode_2x;
    case xenos::MsaaSamples::k4X:
      return DkMsMode_4x;
    default:
      return DkMsMode_1x;
  }
}

}  // namespace

Deko3DCommandProcessor::Deko3DCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state,
                                               ui::deko3d::Deko3DDevice* device)
    : CommandProcessor(graphics_system, kernel_state), device_(device) {}

Deko3DCommandProcessor::~Deko3DCommandProcessor() = default;

bool Deko3DCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    return false;
  }
  if (!device_) {
    REXGPU_ERROR("deko3d: no shared device for the command processor");
    return false;
  }
  // Every step below is logged and flushed. The Switch port runs close to its
  // Horizon memory pool limit, so which allocation stopped the renderer has to
  // survive in the log rather than sit in a buffer the process never reaches.
  REXGPU_INFO("deko3d: command processor setting up");
  device_->LogMemory("gpu setup: shared memory");
  shared_memory_ = std::make_unique<Deko3DSharedMemory>(*device_, *memory_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("deko3d: failed to initialize shared memory");
    shared_memory_.reset();
    return false;
  }
  device_->LogMemory("gpu setup: render target cache");
  render_target_cache_ = std::make_unique<Deko3DRenderTargetCache>(
      *register_file_, *memory_, *device_, *shared_memory_);
  if (!render_target_cache_->Initialize()) {
    REXGPU_ERROR("deko3d: failed to initialize the render target cache");
    render_target_cache_.reset();
    shared_memory_.reset();
    return false;
  }
  texture_cache_ = std::make_unique<Deko3DTextureCache>(*register_file_, *shared_memory_,
                                                       *device_);
  if (!texture_cache_->Initialize()) {
    REXGPU_ERROR("deko3d: failed to initialize the texture cache");
    texture_cache_.reset();
    render_target_cache_->Shutdown();
    render_target_cache_.reset();
    shared_memory_.reset();
    return false;
  }
  device_->LogMemory("gpu setup: texture cache");
  shader_code_ = std::make_unique<Deko3DShaderCodeArena>(*device_);
  shader_code_->set_per_warp_scratch_size(ui::deko3d::Deko3DDevice::kPerWarpScratchSize);

  constant_ring_ = std::make_unique<Deko3DGpuRing>(*device_, kConstantRingSize, "constant");
  descriptor_ring_ = std::make_unique<Deko3DGpuRing>(*device_, kDescriptorRingSize, "descriptor");
  if (!constant_ring_->Initialize() || !descriptor_ring_->Initialize()) {
    REXGPU_ERROR("deko3d: failed to allocate the draw rings");
    return false;
  }

  device_->LogMemory("gpu setup: primitive processor");
  primitive_processor_ = std::make_unique<Deko3DPrimitiveProcessor>(
      *register_file_, *memory_, *shared_memory_, *device_);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("deko3d: failed to initialize the primitive processor");
    primitive_processor_.reset();
    return false;
  }

  if (!InitializePlaceholderTexture()) {
    REXGPU_ERROR("deko3d: failed to create the placeholder texture");
    return false;
  }
  device_->LogMemory("gpu setup: done");

  // The rings may only be recycled once the GPU has finished the previous
  // command list, which is exactly when the render target cache opens a new one.
  render_target_cache_->set_recording_reset_callback([this]() { ResetPerFrameRings(); });
  return true;
}

void Deko3DCommandProcessor::ShutdownContext() {
  if (render_target_cache_) {
    render_target_cache_->set_recording_reset_callback(nullptr);
  }
  if (device_) {
    device_->WaitIdle();
  }
  primitive_processor_.reset();
  shaders_.clear();
  shader_code_.reset();
  placeholder_image_ = {};
  placeholder_layout_ = {};
  placeholder_memory_ = nullptr;
  descriptor_ring_.reset();
  constant_ring_.reset();
  if (texture_cache_) {
    texture_cache_->Shutdown();
    texture_cache_.reset();
  }
  if (render_target_cache_) {
    render_target_cache_->Flush();
    render_target_cache_->Shutdown();
    render_target_cache_.reset();
  }
  shared_memory_.reset();
}

bool Deko3DCommandProcessor::InitializePlaceholderTexture() {
  REXGPU_WARN("deko3d: placeholder texture: laying out 1x1 RGBA8");
  dk::ImageLayoutMaker maker{device_->device()};
  maker.setFlags(DkImageFlags_Usage2DEngine)
      .setType(DkImageType_2D)
      .setFormat(DkImageFormat_RGBA8_Unorm)
      .setDimensions(1, 1, 1);
  maker.initialize(placeholder_layout_);
  const uint64_t size = placeholder_layout_.getSize();
  const uint32_t alignment = placeholder_layout_.getAlignment();
  if (!size || !alignment) {
    REXGPU_WARN("deko3d: placeholder texture: no layout size/alignment");
    return false;
  }
  // Round up to the image's own alignment, then to a page: deko3d memory blocks
  // must be whole pages.
  uint64_t block_size = (size + alignment - 1) & ~uint64_t(alignment - 1);
  block_size = (block_size + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint64_t(DK_MEMBLOCK_ALIGNMENT - 1);
  placeholder_memory_ = dk::MemBlockMaker{device_->device(), uint32_t(block_size)}
                            .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached |
                                      DkMemBlockFlags_Image)
                            .create();
  if (!placeholder_memory_) {
    return false;
  }
  // Opaque white, so a texture that could not be loaded shows up rather than
  // sampling garbage.
  uint32_t* pixels = static_cast<uint32_t*>(placeholder_memory_.getCpuAddr());
  pixels[0] = 0xFFFFFFFFu;
  FlushCpuRange(placeholder_memory_, 0, sizeof(uint32_t));
  placeholder_image_.initialize(placeholder_layout_, placeholder_memory_, 0);
  REXGPU_WARN("deko3d: placeholder texture ready");
  return true;
}

void Deko3DCommandProcessor::ResetPerFrameRings() {
  if (constant_ring_) {
    constant_ring_->Reset();
  }
  if (descriptor_ring_) {
    descriptor_ring_->Reset();
  }
  if (primitive_processor_) {
    primitive_processor_->ResetPerFrame();
  }
  cached_system_constants_.valid = false;
  cached_float_constants_vertex_.valid = false;
  cached_float_constants_pixel_.valid = false;
  cached_bool_loop_constants_.valid = false;
  cached_fetch_constants_.valid = false;
  std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
  std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
}

void Deko3DCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  shader_pack_ = std::make_unique<Deko3DShaderPack>();
  const char* candidates[] = {
      "Game_files/native_shaders/shaders.dk.pak",
      "native_shaders/shaders.dk.pak",
  };
  for (const char* candidate : candidates) {
    if (shader_pack_->Load(candidate)) {
      return;
    }
  }
  REXGPU_WARN(
      "deko3d: no shader pack found; draws will be skipped until "
      "Game_files/native_shaders/shaders.dk.pak exists (generate it with "
      "tools/deko3d/convert_shaders.py and pack_shaders.py)");
  shader_pack_.reset();
}

Shader* Deko3DCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  if (!host_address || !dword_count) {
    return nullptr;
  }
  // The same hash the dump_shaders aid names its files with, so the pack keys
  // (<ps|vs>_<hash>_<modification>) match directly.
  const uint64_t ucode_hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(ucode_hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  // The DKSH variant is chosen per draw, from the translator modification, so
  // the shader object itself only carries the microcode (which the constant
  // upload and the modification both need analyzed).
  std::unique_ptr<Deko3DShader> shader = std::make_unique<Deko3DShader>(
      shader_type, ucode_hash, host_address, dword_count, shader_pack_.get());
  Deko3DShader* shader_ptr = shader.get();
  shaders_.emplace(ucode_hash, std::move(shader));
  static uint32_t loaded_shaders = 0;
  if (++loaded_shaders <= 8 || (loaded_shaders & 63) == 0) {
    REXGPU_INFO("deko3d: loaded shader {} ({} dwords)", shader_ptr->pack_prefix(), dword_count);
  }
  return shader_ptr;
}

uint64_t Deko3DCommandProcessor::GetVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  const RegisterFile& regs = *register_file_;
  SpirvShaderTranslator::Modification modification;
  modification.vertex.dynamic_addressable_register_count =
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg);
  modification.vertex.host_vertex_shader_type = host_vertex_shader_type;
  modification.vertex.interpolator_mask = interpolator_mask;
  modification.vertex.tessellation_mode = 0;

  const auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  // Clip and cull distances are both available in the translated shaders, so
  // user clip planes are never demoted to cull distances here.
  modification.vertex.user_clip_plane_cull =
      (user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena) ? 1 : 0;
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;

  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  }
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);
  return modification.value;
}

uint64_t Deko3DCommandProcessor::GetPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  const RegisterFile& regs = *register_file_;
  SpirvShaderTranslator::Modification modification;
  modification.pixel.dynamic_addressable_register_count =
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg);
  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                              regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  // Host render targets, with the depth stored in the host's native format
  // rather than converted to float24 in the pixel shader - which is what the
  // shader pack was generated with.
  using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
  if (shader.implicit_early_z_write_allowed() &&
      (!shader.writes_color_target(0) ||
       !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }
  (void)normalized_depth_control;
  return modification.value;
}

void Deko3DCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask) {
  (void)normalized_color_mask;
  const RegisterFile& regs = *register_file_;
  const auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  const auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  const auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  const auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  const auto vgt_max_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  const auto vgt_min_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);

  // The fragment-shader-interlock (EDRAM emulation) paths of the Vulkan
  // version are not ported: this backend always uses host render targets, so
  // blending, depth, stencil and polygon offset are all fixed-function.
  uint32_t flags = 0;
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    // Point and rectangle expansion uses primitive restart only in the host
    // built-in strip index buffer; guest indices are not reset-based there.
    if (primitive_processing_result.host_primitive_reset_enabled &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }
  // W0 division control.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  const xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma render targets are stored as unorm RGBA8 here, so the shader does the
  // Xenos PWL gamma conversion rather than the host attachment format.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
  }
  system_constants_.flags = flags;

  if (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad) {
    system_constants_.vertex_index_load_address = primitive_processing_result.guest_index_base;
  }
  system_constants_.vertex_index_reset =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset)
          ? regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
          : 0;
  system_constants_.compute_memexport_vertex_count = 0;

  // Index endianness for shader-side index loading.
  xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 &&
      guest_index_endian != xenos::Endian::kNone && guest_index_endian != xenos::Endian::k8in16) {
    guest_index_endian =
        guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  system_constants_.vertex_index_endian =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)
          ? guest_index_endian
          : primitive_processing_result.host_shader_index_endian;
  system_constants_.line_loop_closing_index = primitive_processing_result.line_loop_closing_index;
  system_constants_.vertex_base_index = vgt_indx_offset;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      std::memcpy(user_clip_plane_write_ptr,
                  &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4],
                  4 * sizeof(float));
      user_clip_plane_write_ptr += 4;
    }
  }

  system_constants_.tessellation_factor_range_min =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  system_constants_.tessellation_factor_range_max =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;

  for (uint32_t i = 0; i < 3; ++i) {
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    const auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    const auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    system_constants_.point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    system_constants_.point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    system_constants_.point_constant_diameter[0] = float(pa_su_point_size.width) * (2.0f / 16.0f);
    system_constants_.point_constant_diameter[1] = float(pa_su_point_size.height) * (2.0f / 16.0f);
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion.
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        1.0f / std::max(viewport_info.xy_extent[0], uint32_t(1));
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        1.0f / std::max(viewport_info.xy_extent[1], uint32_t(1));
  }

  // Texture signedness, and whether any texture is resolution-scaled (never,
  // here: this backend does not scale the draw resolution).
  uint32_t textures_resolution_scaled = 0;
  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
      const uint32_t texture_signs_shift = 8 * (texture_index & 3);
      const uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      texture_signs_uint = (texture_signs_uint & ~(((UINT32_C(1) << 8) - 1) << texture_signs_shift)) |
                           (uint32_t(texture_signs) << texture_signs_shift);
      textures_resolution_scaled |=
          uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
    }
  }
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;
  // Host swizzles are applied through the image view's component swizzle on
  // this backend, not in the shader, so texture_swizzles stays zero.
  std::memset(system_constants_.texture_swizzles, 0, sizeof(system_constants_.texture_swizzles));

  system_constants_.alpha_test_reference = rb_alpha_ref;
  system_constants_.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                                        ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8)
                                        : 0;

  // Color exponent bias. The 16-bit unorm targets are treated as truncated to
  // the host's range (like the Vulkan backend's SNORM16 targets), so no extra
  // rescaling is applied.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    const int32_t color_exp_bias = color_info.color_exp_bias;
    float color_exp_bias_scale;
    uint32_t color_exp_bias_bits = UINT32_C(0x3F800000) + (uint32_t(color_exp_bias) << 23);
    std::memcpy(&color_exp_bias_scale, &color_exp_bias_bits, sizeof(float));
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }

  (void)normalized_depth_control;
  (void)pa_su_sc_mode_cntl;
}

bool Deko3DCommandProcessor::UploadConstants(CachedConstants& cache, const void* data, size_t size,
                                             DkGpuAddr& address_out) {
  address_out = 0;
  if (cache.valid && cache.bytes.size() == size &&
      std::memcmp(cache.bytes.data(), data, size) == 0) {
    address_out = cache.address;
    return true;
  }
  DkGpuAddr address = 0;
  void* mapping = constant_ring_->Request(uint32_t(size), kBufferAlignment, address);
  if (!mapping) {
    REXGPU_ERROR("deko3d: constant ring full ({} bytes used of {}); the frame's remaining "
                 "draws are skipped",
                 constant_ring_->used(), constant_ring_->size());
    return false;
  }
  std::memcpy(mapping, data, size);
  cache.bytes.assign(static_cast<const uint8_t*>(data),
                     static_cast<const uint8_t*>(data) + size);
  cache.address = address;
  cache.valid = true;
  address_out = address;
  return true;
}

bool Deko3DCommandProcessor::UploadConstantBuffers(const Deko3DShader& vertex_shader,
                                                   const Deko3DShader* pixel_shader,
                                                   ConstantBuffers& buffers_out) {
  const RegisterFile& regs = *register_file_;
  buffers_out = ConstantBuffers();

  if (!UploadConstants(cached_system_constants_, &system_constants_, sizeof(system_constants_),
                       buffers_out.system_constants)) {
    return false;
  }

  // Float constants are tightly packed: only the registers the shader actually
  // reads, in ascending order (see Shader::ConstantRegisterMap).
  const Shader::ConstantRegisterMap& float_map_vertex = vertex_shader.constant_register_map();
  const uint32_t float_count_vertex = std::max(float_map_vertex.float_count, uint32_t(1));
  std::vector<uint8_t> float_bytes(size_t(float_count_vertex) * sizeof(float) * 4);
  uint8_t* float_write = float_bytes.data();
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t float_constant_map_entry = float_map_vertex.float_bitmap[i];
    uint32_t float_constant_index;
    while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
      float_constant_map_entry &= ~(1ull << float_constant_index);
      std::memcpy(float_write,
                  &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (float_constant_index << 2)],
                  sizeof(float) * 4);
      float_write += sizeof(float) * 4;
    }
  }
  if (!UploadConstants(cached_float_constants_vertex_, float_bytes.data(), float_bytes.size(),
                       buffers_out.float_vertex)) {
    return false;
  }
  buffers_out.float_vertex_size = uint32_t(float_bytes.size());

  uint32_t float_count_pixel = 1;
  if (pixel_shader) {
    const Shader::ConstantRegisterMap& float_map_pixel = pixel_shader->constant_register_map();
    float_count_pixel = std::max(float_map_pixel.float_count, uint32_t(1));
    float_bytes.resize(size_t(float_count_pixel) * sizeof(float) * 4);
    float_write = float_bytes.data();
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t float_constant_map_entry = float_map_pixel.float_bitmap[i];
      uint32_t float_constant_index;
      while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
        float_constant_map_entry &= ~(1ull << float_constant_index);
        std::memcpy(
            float_write,
            &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (float_constant_index << 2)],
            sizeof(float) * 4);
        float_write += sizeof(float) * 4;
      }
    }
  } else {
    float_bytes.assign(sizeof(float) * 4, 0);
  }
  if (!UploadConstants(cached_float_constants_pixel_, float_bytes.data(), float_bytes.size(),
                       buffers_out.float_pixel)) {
    return false;
  }
  buffers_out.float_pixel_size = uint32_t(float_bytes.size());

  if (!UploadConstants(cached_bool_loop_constants_, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                       kBoolLoopConstantsSize, buffers_out.bool_loop)) {
    return false;
  }
  return UploadConstants(cached_fetch_constants_, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                         kFetchConstantsSize, buffers_out.fetch);
}

bool Deko3DCommandProcessor::BindTextures(dk::CmdBuf& command_buffer,
                                          const Deko3DShader::HostShader& vertex_host_shader,
                                          const Deko3DShader::HostShader* pixel_host_shader) {
  const uint32_t pixel_count =
      pixel_host_shader ? uint32_t(pixel_host_shader->texture_bindings.size()) : 0;
  const uint32_t vertex_count = uint32_t(vertex_host_shader.texture_bindings.size());
  const uint32_t total = pixel_count + vertex_count;
  if (!total || total > 2 * kMaxTextureBindingsPerStage) {
    return total == 0;
  }

  // The image and sampler descriptor sets are global to the queue, so the pixel
  // shader's bindings take the first slots and the vertex shader's follow.
  // deko3d does not document a descriptor-set alignment; page-fraction
  // alignment is what its own samples effectively end up with, and the cost of
  // over-aligning is a few hundred bytes a draw.
  DkGpuAddr image_set_address = 0;
  auto* image_descriptors = static_cast<DkImageDescriptor*>(
      descriptor_ring_->Request(total * kDescriptorSize, kDescriptorSetAlignment,
                                image_set_address));
  DkGpuAddr sampler_set_address = 0;
  auto* sampler_descriptors = static_cast<DkSamplerDescriptor*>(
      descriptor_ring_->Request(total * kDescriptorSize, kDescriptorSetAlignment,
                                sampler_set_address));
  if (!image_descriptors || !sampler_descriptors) {
    REXGPU_ERROR("deko3d: descriptor ring full ({} bytes used of {}); the frame's remaining "
                 "draws are skipped",
                 descriptor_ring_->used(), descriptor_ring_->size());
    return false;
  }

  const RegisterFile& regs = *register_file_;
  DkResHandle pixel_handles[kMaxTextureBindingsPerStage];
  DkResHandle vertex_handles[kMaxTextureBindingsPerStage];
  for (uint32_t i = 0; i < total; ++i) {
    const bool is_pixel = i < pixel_count;
    const Deko3DTextureBinding& binding =
        is_pixel ? pixel_host_shader->texture_bindings[i]
                 : vertex_host_shader.texture_bindings[i - pixel_count];

    Deko3DTextureCache::BoundTexture bound;
    const bool want_signed = binding.is_signed != 0;
    if (!texture_cache_->GetBoundTexture(binding.texture_fetch_constant, want_signed, bound)) {
      bound = Deko3DTextureCache::BoundTexture();
      bound.image = &placeholder_image_;
      bound.image_type = DkImageType_2D;
      bound.depth_or_array_size = 1;
    }

    dk::ImageView image_view{*bound.image};
    // The translated shaders always sample through an array type, so a 2D host
    // image is viewed as a one-layer array.
    switch (binding.dimension) {
      case 1:
        image_view.setType(DkImageType_Cubemap);
        image_view.setLayers(0, uint16_t(std::max(bound.depth_or_array_size, uint32_t(1))));
        break;
      case 2:
        image_view.setType(DkImageType_3D);
        break;
      default:
        image_view.setType(bound.image_type == DkImageType_2DArray ? DkImageType_2DArray
                                                                   : DkImageType_2DArray);
        image_view.setLayers(0, bound.image_type == DkImageType_2DArray
                                    ? uint16_t(std::max(bound.depth_or_array_size, uint32_t(1)))
                                    : uint16_t(1));
        break;
    }
    dkImageDescriptorInitialize(&image_descriptors[i], &image_view, false, false);

    const xenos::xe_gpu_texture_fetch_t fetch =
        regs.GetTextureFetch(binding.sampler_fetch_constant);
    xenos::ClampMode clamp_x, clamp_y, clamp_z;
    texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);

    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    const xenos::TextureFilter mag_filter =
        binding.mag_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mag_filter
                                                                   : binding.mag_filter;
    const xenos::TextureFilter min_filter =
        binding.min_filter == xenos::TextureFilter::kUseFetchConst ? fetch.min_filter
                                                                   : binding.min_filter;
    const xenos::TextureFilter mip_filter =
        binding.mip_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mip_filter
                                                                   : binding.mip_filter;
    sampler.magFilter = mag_filter == xenos::TextureFilter::kLinear ? DkFilter_Linear
                                                                    : DkFilter_Nearest;
    sampler.minFilter = min_filter == xenos::TextureFilter::kLinear ? DkFilter_Linear
                                                                    : DkFilter_Nearest;
    // kBaseMap means "no mip mapping" - only the base level is sampled.
    sampler.mipFilter = mip_filter == xenos::TextureFilter::kBaseMap
                            ? DkMipFilter_None
                            : (mip_filter == xenos::TextureFilter::kLinear ? DkMipFilter_Linear
                                                                           : DkMipFilter_Nearest);
    sampler.wrapMode[0] = GetDkWrapMode(clamp_x);
    sampler.wrapMode[1] = GetDkWrapMode(clamp_y);
    sampler.wrapMode[2] = GetDkWrapMode(clamp_z);
    sampler.lodClampMin = float(fetch.mip_min_level);
    sampler.lodClampMax = mip_filter == xenos::TextureFilter::kBaseMap
                              ? float(fetch.mip_min_level) + 0.25f
                              : 1000.0f;
    // LOD biasing is done in the translated shader, not in the sampler.
    sampler.lodBias = 0.0f;
    xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                          ? fetch.aniso_filter
                                          : binding.aniso_filter;
    sampler.maxAnisotropy =
        aniso_filter == xenos::AnisoFilter::kDisabled ||
                aniso_filter == xenos::AnisoFilter::kUseFetchConst
            ? 1.0f
            : float(UINT32_C(1) << (uint32_t(aniso_filter) -
                                    uint32_t(xenos::AnisoFilter::kMax_1_1)));
    if (xenos::ClampModeUsesBorder(clamp_x) || xenos::ClampModeUsesBorder(clamp_y) ||
        xenos::ClampModeUsesBorder(clamp_z)) {
      const float border = fetch.border_color == xenos::BorderColor::k_ABGR_White ? 1.0f : 0.0f;
      // The alpha of the black border color is 1 on the Xbox 360.
      const float border_alpha =
          fetch.border_color == xenos::BorderColor::k_ABGR_Black ? 1.0f : border;
      sampler.borderColor[0].value_f = border;
      sampler.borderColor[1].value_f = border;
      sampler.borderColor[2].value_f = border;
      sampler.borderColor[3].value_f = border_alpha;
    }
    dkSamplerDescriptorInitialize(&sampler_descriptors[i], &sampler);

    const DkResHandle handle = dkMakeTextureHandle(i, i);
    if (is_pixel) {
      pixel_handles[i] = handle;
    } else {
      vertex_handles[i - pixel_count] = handle;
    }
  }

  const DkCmdBuf cmd = command_buffer;
  command_buffer.bindImageDescriptorSet(image_set_address, total);
  command_buffer.bindSamplerDescriptorSet(sampler_set_address, total);
  if (pixel_count) {
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, pixel_handles, pixel_count);
  }
  if (vertex_count) {
    dkCmdBufBindTextures(cmd, DkStage_Vertex, 0, vertex_handles, vertex_count);
  }
  return true;
}

bool Deko3DCommandProcessor::BindRenderTargets(dk::CmdBuf& command_buffer,
                                               uint32_t normalized_color_mask,
                                               uint32_t& color_target_count_out) {
  color_target_count_out = 0;
  Deko3DRenderTargetCache::Target* depth_target = render_target_cache_->GetUsedRenderTarget(0);

  // deko3d takes a contiguous array of color attachments, so bind every target
  // the render target cache resolved for this draw, in order. A target whose
  // write mask is zero is still attached: dropping it would shift the indices
  // of the ones after it, and the pixel shader writes by location.
  const DkImageView* color_views[xenos::kMaxColorRenderTargets];
  DkImageView storage[xenos::kMaxColorRenderTargets];
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    Deko3DRenderTargetCache::Target* target = render_target_cache_->GetUsedRenderTarget(1 + i);
    if (!target || !target->valid()) {
      break;
    }
    storage[color_count] = dk::ImageView{target->image()};
    color_views[color_count] = &storage[color_count];
    ++color_count;
  }
  (void)normalized_color_mask;
  if (!color_count && !depth_target) {
    return false;
  }

  DkImageView depth_storage;
  const DkImageView* depth_view = nullptr;
  if (depth_target && depth_target->valid()) {
    depth_storage = dk::ImageView{depth_target->image()};
    depth_view = &depth_storage;
  }
  dkCmdBufBindRenderTargets(command_buffer, color_views, color_count, depth_view);
  color_target_count_out = color_count;
  return color_count != 0 || depth_view != nullptr;
}

void Deko3DCommandProcessor::BindPipelineState(
    dk::CmdBuf& command_buffer, bool primitive_polygonal,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    uint32_t color_target_count, const draw_util::ViewportInfo& viewport_info,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result) {
  const RegisterFile& regs = *register_file_;
  const auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();

  // Rasterizer.
  DkRasterizerState rasterizer;
  dkRasterizerStateDefaults(&rasterizer);
  rasterizer.rasterizerEnable = 1;
  rasterizer.depthClampEnable = pa_cl_clip_cntl.clip_disable ? 1 : 0;
  rasterizer.frontFace = pa_su_sc_mode_cntl.face ? DkFrontFace_CW : DkFrontFace_CCW;
  rasterizer.provokingVertex = pa_su_sc_mode_cntl.provoking_vtx_last ? DkProvokingVertex_Last
                                                                     : DkProvokingVertex_First;
  if (primitive_polygonal) {
    rasterizer.cullMode = DkFace_None;
    if (pa_su_sc_mode_cntl.cull_front) {
      rasterizer.cullMode = DkFace(rasterizer.cullMode | DkFace_Front);
    }
    if (pa_su_sc_mode_cntl.cull_back) {
      rasterizer.cullMode = DkFace(rasterizer.cullMode | DkFace_Back);
    }
    xenos::PolygonType polygon_type = xenos::PolygonType::kTriangles;
    if (pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
      polygon_type = pa_su_sc_mode_cntl.polymode_front_ptype;
      if (!pa_su_sc_mode_cntl.cull_front) {
        polygon_type = std::min(polygon_type, pa_su_sc_mode_cntl.polymode_back_ptype);
      }
    }
    const DkPolygonMode polygon_mode = polygon_type == xenos::PolygonType::kTriangles
                                           ? DkPolygonMode_Fill
                                           : DkPolygonMode_Line;
    rasterizer.polygonModeFront = polygon_mode;
    rasterizer.polygonModeBack = polygon_mode;
  } else {
    rasterizer.cullMode = DkFace_None;
    rasterizer.polygonModeFront = DkPolygonMode_Fill;
    rasterizer.polygonModeBack = DkPolygonMode_Fill;
  }
  // Polygon offset is applied with setDepthBias below and only has an effect
  // when a depth attachment is bound.
  const bool depth_bias_enabled = primitive_processing_result.host_vertex_shader_type ==
                                      Shader::HostVertexShaderType::kVertex &&
                                  (primitive_polygonal ? (pa_su_sc_mode_cntl.poly_offset_front_enable ||
                                                          pa_su_sc_mode_cntl.poly_offset_back_enable)
                                                       : pa_su_sc_mode_cntl.poly_offset_para_enable);
  rasterizer.depthBiasEnableMask = depth_bias_enabled ? DkPolygonFlag_All : 0;
  command_buffer.bindRasterizerState(rasterizer);

  if (depth_bias_enabled) {
    float depth_bias_constant_factor, depth_bias_slope_factor;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, depth_bias_slope_factor,
                                             depth_bias_constant_factor);
    depth_bias_constant_factor *=
        regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
            ? draw_util::kD3D10PolygonOffsetFactorUnorm24
            : draw_util::kD3D10PolygonOffsetFactorFloat24;
    depth_bias_slope_factor *= xenos::kPolygonOffsetScaleSubpixelUnit;
    command_buffer.setDepthBias(depth_bias_constant_factor, 0.0f, depth_bias_slope_factor);
  } else {
    command_buffer.setDepthBias(0.0f, 0.0f, 0.0f);
  }

  // Multisample.
  DkMultisampleState multisample;
  dkMultisampleStateDefaults(&multisample);
  multisample.mode = GetMsMode(rb_surface_info.msaa_samples);
  multisample.rasterizerMode = multisample.mode;
  multisample.alphaToCoverageEnable = rb_colorcontrol.alpha_to_mask_enable ? 1 : 0;
  command_buffer.bindMultisampleState(multisample);

  // Color, blending and the write masks.
  DkColorState color_state;
  dkColorStateDefaults(&color_state);
  // Alpha test is performed by the translated pixel shader from the system
  // constants, so the host's alpha compare stays disabled.
  color_state.alphaCompareOp = DkCompareOp_Always;
  DkColorWriteState color_write;
  dkColorWriteStateDefaults(&color_write);
  DkBlendState blend_states[xenos::kMaxColorRenderTargets];
  uint32_t blend_enable_mask = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    dkBlendStateDefaults(&blend_states[i]);
    const uint32_t write_mask = (normalized_color_mask >> (i * 4)) & 0b1111;
    dkColorWriteStateSetMask(&color_write, i, write_mask);
    if (i >= color_target_count) {
      continue;
    }
    const auto blend_control =
        regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
    // The guest disables blending by writing 1 * src + 0 * dst with add.
    const bool blend_enabled =
        blend_control.color_srcblend != xenos::BlendFactor::kOne ||
        blend_control.color_destblend != xenos::BlendFactor::kZero ||
        blend_control.color_comb_fcn != xenos::BlendOp::kAdd ||
        blend_control.alpha_srcblend != xenos::BlendFactor::kOne ||
        blend_control.alpha_destblend != xenos::BlendFactor::kZero ||
        blend_control.alpha_comb_fcn != xenos::BlendOp::kAdd;
    if (!blend_enabled) {
      continue;
    }
    blend_enable_mask |= 1u << i;
    dkBlendStateSetOps(&blend_states[i], GetDkBlendOp(blend_control.color_comb_fcn),
                       GetDkBlendOp(blend_control.alpha_comb_fcn));
    dkBlendStateSetFactors(&blend_states[i], GetDkBlendFactor(blend_control.color_srcblend),
                           GetDkBlendFactor(blend_control.color_destblend),
                           GetDkBlendFactor(blend_control.alpha_srcblend),
                           GetDkBlendFactor(blend_control.alpha_destblend));
  }
  color_state.blendEnableMask = blend_enable_mask;
  command_buffer.bindColorState(color_state);
  command_buffer.bindColorWriteState(color_write);
  dkCmdBufBindBlendStates(command_buffer, 0, blend_states,
                          color_target_count ? color_target_count : 1);
  command_buffer.setBlendConst(regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
                               regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                               regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
                               regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA));

  // Depth and stencil.
  DkDepthStencilState depth_stencil;
  dkDepthStencilStateDefaults(&depth_stencil);
  depth_stencil.depthTestEnable = normalized_depth_control.z_enable ? 1 : 0;
  depth_stencil.depthWriteEnable =
      (normalized_depth_control.z_enable && normalized_depth_control.z_write_enable) ? 1 : 0;
  depth_stencil.depthCompareOp = GetDkCompareOp(normalized_depth_control.zfunc);
  depth_stencil.stencilTestEnable = normalized_depth_control.stencil_enable ? 1 : 0;
  depth_stencil.stencilFrontCompareOp = GetDkCompareOp(normalized_depth_control.stencilfunc);
  depth_stencil.stencilFrontFailOp = GetDkStencilOp(normalized_depth_control.stencilfail);
  depth_stencil.stencilFrontPassOp = GetDkStencilOp(normalized_depth_control.stencilzpass);
  depth_stencil.stencilFrontDepthFailOp = GetDkStencilOp(normalized_depth_control.stencilzfail);
  const bool use_back_stencil =
      primitive_polygonal && normalized_depth_control.backface_enable;
  depth_stencil.stencilBackCompareOp =
      GetDkCompareOp(use_back_stencil ? normalized_depth_control.stencilfunc_bf
                                      : normalized_depth_control.stencilfunc);
  depth_stencil.stencilBackFailOp =
      GetDkStencilOp(use_back_stencil ? normalized_depth_control.stencilfail_bf
                                      : normalized_depth_control.stencilfail);
  depth_stencil.stencilBackPassOp =
      GetDkStencilOp(use_back_stencil ? normalized_depth_control.stencilzpass_bf
                                      : normalized_depth_control.stencilzpass);
  depth_stencil.stencilBackDepthFailOp =
      GetDkStencilOp(use_back_stencil ? normalized_depth_control.stencilzfail_bf
                                      : normalized_depth_control.stencilzfail);
  command_buffer.bindDepthStencilState(depth_stencil);

  const auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
  command_buffer.setStencil(DkFace_Front, uint8_t(stencil_ref_mask.stencilwritemask),
                            uint8_t(stencil_ref_mask.stencilref),
                            uint8_t(stencil_ref_mask.stencilmask));
  if (use_back_stencil) {
    const auto stencil_ref_mask_bf =
        regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
    command_buffer.setStencil(DkFace_Back, uint8_t(stencil_ref_mask_bf.stencilwritemask),
                              uint8_t(stencil_ref_mask_bf.stencilref),
                              uint8_t(stencil_ref_mask_bf.stencilmask));
  }

  // Viewport and scissor. This deko3d device is created with the upper-left
  // origin and 0...1 depth, which is what the Vulkan-targeted shaders and
  // GetHostViewportInfo already produce, so no flipping is needed.
  DkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = float(viewport_info.xy_offset[0]);
    viewport.y = float(viewport_info.xy_offset[1]);
    viewport.width = float(viewport_info.xy_extent[0]);
    viewport.height = float(viewport_info.xy_extent[1]);
  } else {
    // An empty guest viewport: park it outside the render target.
    viewport.x = -1.0f;
    viewport.y = -1.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
  }
  viewport.near = viewport_info.z_min;
  viewport.far = viewport_info.z_max;
  dkCmdBufSetViewports(command_buffer, 0, &viewport, 1);

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  DkScissor dk_scissor{scissor.offset[0], scissor.offset[1], scissor.extent[0], scissor.extent[1]};
  dkCmdBufSetScissors(command_buffer, 0, &dk_scissor, 1);

  // The vertex shaders fetch from the shared memory buffer, so no vertex
  // attribute or buffer state may be left bound from anything else on this
  // queue.
  dkCmdBufBindVtxAttribState(command_buffer, nullptr, 0);
  dkCmdBufBindVtxBufferState(command_buffer, nullptr, 0);

  command_buffer.setPrimitiveRestart(
      primitive_processing_result.host_primitive_reset_enabled,
      primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16 ? 0xFFFFu
                                                                                  : 0xFFFFFFFFu);
}

bool Deko3DCommandProcessor::DrawSkipped(const char* reason) {
  ++frame_skips_;
  const uint32_t count = ++skip_reasons_[reason];
  if (count <= 3 || (count & 1023) == 0) {
    REXGPU_WARN("deko3d: draw skipped ({}) - occurrence {}", reason, count);
  }
  return true;
}

void Deko3DCommandProcessor::ReportFrameDrawStats() {
  if (!frame_draws_ && !frame_skips_) {
    return;
  }
  static uint32_t logged_frames = 0;
  if (++logged_frames <= 5 || (logged_frames & 127) == 0) {
    std::string reasons;
    for (const auto& reason : skip_reasons_) {
      if (!reasons.empty()) {
        reasons += ", ";
      }
      reasons += reason.first + " x" + std::to_string(reason.second);
    }
    REXGPU_INFO("deko3d: frame {} drew {}, skipped {}{}", frame_index_, frame_draws_, frame_skips_,
                reasons.empty() ? "" : (" (" + reasons + ")"));
  }
  frame_draws_ = 0;
  frame_skips_ = 0;
  skip_reasons_.clear();
}

bool Deko3DCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  const RegisterFile& regs = *register_file_;

  // Resolves are submitted as draw packets with EDRAM copy mode (the Vulkan and
  // D3D12 backends dispatch the same way). Without this check the guest's
  // resolve draws are silently dropped.
  const xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }

  // Open this frame's command list first. Opening it waits for the previous
  // frame's fence and recycles the per-frame rings, and everything below writes
  // to those rings or to the shared memory mirror (primitive conversion,
  // texture uploads, constants) - all of which the previous frame's list may
  // still be reading.
  dk::CmdBuf* command_buffer = render_target_cache_->BeginRecording();
  if (!command_buffer) {
    return DrawSkipped("command_buffer");
  }

  const bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  const bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0 && is_rasterization_done) {
    // Doesn't actually draw.
    return true;
  }

  auto* vertex_shader = static_cast<Deko3DShader*>(active_vertex_shader());
  if (!vertex_shader) {
    return DrawSkipped("missing_vertex_shader");
  }
  Deko3DShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<Deko3DShader*>(active_pixel_shader());
    }
  } else {
    // Without rasterization a draw can only matter for memory export, which this
    // backend does not implement.
    return DrawSkipped("no_rasterization");
  }

  if (!vertex_shader->is_ucode_analyzed()) {
    vertex_shader->AnalyzeUcode(ucode_disasm_buffer_);
  }
  if (pixel_shader && !pixel_shader->is_ucode_analyzed()) {
    pixel_shader->AnalyzeUcode(ucode_disasm_buffer_);
  }
  if (vertex_shader->memexport_eM_written() ||
      (pixel_shader && pixel_shader->memexport_eM_written())) {
    return DrawSkipped("memexport");
  }
  if (pixel_shader && !draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
    pixel_shader = nullptr;
  }

  const reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  uint32_t ps_param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    return DrawSkipped("primitive_processing");
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }
  // Every other host vertex shader type needs its own translated vertex shader
  // (point and rectangle expansion, tessellation); the pack lookup below reports
  // the missing key when the game uses one.
  const uint64_t vertex_modification = GetVertexShaderModification(
      *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask,
      ps_param_gen_pos != UINT32_MAX);
  const uint64_t pixel_modification =
      pixel_shader ? GetPixelShaderModification(*pixel_shader, interpolator_mask, ps_param_gen_pos,
                                                normalized_depth_control)
                   : 0;

  const Deko3DShader::HostShader* vertex_host_shader =
      vertex_shader->GetHostShader(vertex_modification, *shader_code_);
  if (!vertex_host_shader) {
    return DrawSkipped("vertex_dksh");
  }
  const Deko3DShader::HostShader* pixel_host_shader = nullptr;
  if (pixel_shader) {
    pixel_host_shader = pixel_shader->GetHostShader(pixel_modification, *shader_code_);
    if (!pixel_host_shader) {
      return DrawSkipped("pixel_dksh");
    }
  }

  bool primitive_supported = false;
  const DkPrimitive dk_primitive =
      GetDkPrimitive(primitive_processing_result.host_primitive_type, primitive_supported);
  if (!primitive_supported) {
    return DrawSkipped("primitive_type");
  }

  const uint32_t used_texture_mask =
      vertex_host_shader->used_texture_mask |
      (pixel_host_shader ? pixel_host_shader->used_texture_mask : 0);
  texture_cache_->RequestTextures(used_texture_mask);

  const uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    return DrawSkipped("render_target_update");
  }
  // This backend does not move EDRAM contents between render targets when tile
  // ownership changes. The menu does not alias live targets, but say so loudly
  // if a title ever does, rather than silently drawing stale contents.
  if (const uint32_t pending_transfers = render_target_cache_->GetPendingTransferCount()) {
    static bool logged_transfers = false;
    if (!logged_transfers) {
      logged_transfers = true;
      REXGPU_WARN(
          "deko3d: EDRAM ownership transfer requested ({} ranges); this backend does not "
          "copy between render targets, so aliased surfaces may show stale contents",
          pending_transfers);
    }
  }

  draw_util::ViewportInfo viewport_info;
  // allow_reverse_z is off: deko3d's viewport near/far are not documented to
  // accept an inverted range, and the utility compensates in ndc_scale/offset
  // when it swaps them, so the shaders behave the same.
  draw_util::GetHostViewportInfo(regs, 1, 1, /*origin_bottom_left=*/false, 8192, 8192,
                                 /*allow_reverse_z=*/false, normalized_depth_control,
                                 /*convert_z_to_float24=*/false, /*full_float24_in_0_to_1=*/true,
                                 pixel_shader && pixel_shader->writes_depth(), viewport_info);

  UpdateSystemConstantValues(primitive_polygonal, primitive_processing_result, viewport_info,
                             used_texture_mask, normalized_depth_control, normalized_color_mask);

  ConstantBuffers constant_buffers;
  if (!UploadConstantBuffers(*vertex_shader, pixel_shader, constant_buffers)) {
    return DrawSkipped("constant_upload");
  }

  uint32_t color_target_count = 0;
  if (!BindRenderTargets(*command_buffer, normalized_color_mask, color_target_count)) {
    return DrawSkipped("no_render_targets");
  }

  // Shaders. dk::Shader derives from DkShader, but an array of the derived
  // pointers does not convert to an array of the base ones, so the handles are
  // upcast individually.
  const DkShader* const stages[2] = {&vertex_host_shader->shader,
                                     pixel_host_shader ? &pixel_host_shader->shader : nullptr};
  const uint32_t stage_mask = pixel_host_shader
                                  ? (DkStageFlag_Vertex | DkStageFlag_Fragment)
                                  : DkStageFlag_Vertex;
  dkCmdBufBindShaders(*command_buffer, stage_mask, stages, pixel_host_shader ? 2u : 1u);

  // Constants: storage buffer 0 is the system constants, 1 the 512 MB guest
  // memory mirror the vertex shaders fetch from.
  const DkGpuAddr shared_memory_address = shared_memory_->gpu_address();
  for (DkStage stage : {DkStage_Vertex, DkStage_Fragment}) {
    const bool stage_used = stage == DkStage_Vertex || pixel_host_shader != nullptr;
    if (!stage_used) {
      continue;
    }
    command_buffer->bindStorageBuffer(stage, kStorageBufferSystemConstants,
                                      constant_buffers.system_constants,
                                      uint32_t(sizeof(SpirvShaderTranslator::SystemConstants)));
    command_buffer->bindStorageBuffer(stage, kStorageBufferSharedMemory, shared_memory_address,
                                      uint32_t(SharedMemory::kBufferSize));
    command_buffer->bindUniformBuffer(stage, kUniformBufferBoolLoop, constant_buffers.bool_loop,
                                      uint32_t(kBoolLoopConstantsSize));
    command_buffer->bindUniformBuffer(stage, kUniformBufferFetch, constant_buffers.fetch,
                                      uint32_t(kFetchConstantsSize));
  }
  command_buffer->bindUniformBuffer(DkStage_Vertex, kUniformBufferFloatVertex,
                                    constant_buffers.float_vertex,
                                    constant_buffers.float_vertex_size);
  if (pixel_host_shader) {
    command_buffer->bindUniformBuffer(DkStage_Fragment, kUniformBufferFloatPixel,
                                      constant_buffers.float_pixel,
                                      constant_buffers.float_pixel_size);
  }

  if (!BindTextures(*command_buffer, *vertex_host_shader, pixel_host_shader)) {
    return DrawSkipped("textures");
  }

  BindPipelineState(*command_buffer, primitive_polygonal, normalized_depth_control,
                    normalized_color_mask, color_target_count, viewport_info,
                    primitive_processing_result);

  // Everything the CPU wrote for this draw (constants, descriptors, converted
  // indices) must reach memory before the list is submitted.
  constant_ring_->Flush();
  descriptor_ring_->Flush();
  primitive_processor_->FlushConvertedIndices();

#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(5);
#endif

  switch (primitive_processing_result.index_buffer_type) {
    case PrimitiveProcessor::ProcessedIndexBufferType::kNone:
      command_buffer->draw(dk_primitive, primitive_processing_result.host_draw_vertex_count, 1, 0,
                           0);
      break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
      // Same as the Vulkan backend: the guest's own index buffer is bound
      // straight out of the shared memory mirror. The values are big-endian, so
      // the vertex shader re-loads and swaps them (the
      // kSysFlag_ComputeOrPrimitiveVertexIndexLoad path set from
      // guest_index_base above) - the host buffer only drives the count and the
      // primitive assembly, exactly as it does on Vulkan.
      const uint64_t index_offset = primitive_processing_result.guest_index_base;
      if (index_offset + uint64_t(primitive_processing_result.host_draw_vertex_count) *
                             (primitive_processing_result.host_index_format ==
                                      xenos::IndexFormat::kInt16
                                  ? sizeof(uint16_t)
                                  : sizeof(uint32_t)) >
          SharedMemory::kBufferSize) {
        return DrawSkipped("index_buffer_out_of_range");
      }
      command_buffer->bindIdxBuffer(
          primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
              ? DkIdxFormat_Uint16
              : DkIdxFormat_Uint32,
          shared_memory_->gpu_address() + index_offset);
      command_buffer->drawIndexed(dk_primitive,
                                  primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
      break;
    }
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted: {
      const DkGpuAddr address = primitive_processor_->GetConvertedIndexBufferAddress(
          primitive_processing_result.host_index_buffer_handle);
      if (!address) {
        return DrawSkipped("converted_index_buffer");
      }
      command_buffer->bindIdxBuffer(
          primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
              ? DkIdxFormat_Uint16
              : DkIdxFormat_Uint32,
          address);
      command_buffer->drawIndexed(dk_primitive,
                                  primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
      break;
    }
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA: {
      const DkGpuAddr address = primitive_processor_->GetBuiltinIndexBufferAddress(
          primitive_processing_result.host_index_buffer_handle);
      if (!address) {
        return DrawSkipped("builtin_index_buffer");
      }
      command_buffer->bindIdxBuffer(
          primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
              ? DkIdxFormat_Uint16
              : DkIdxFormat_Uint32,
          address);
      command_buffer->drawIndexed(dk_primitive,
                                  primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
      break;
    }
    default:
      return DrawSkipped("index_buffer_type");
  }

  render_target_cache_->NoteCommandsRecorded();
  render_target_cache_->CountDrawRecorded();
  ++frame_draws_;
  static bool logged_first_draw = false;
  if (!logged_first_draw) {
    logged_first_draw = true;
    // A warning so it is flushed: whether any draw was reached at all is the
    // first question a black screen asks.
    REXGPU_WARN(
        "deko3d: first draw: {} vs {} ps {}, primitive {}, {} vertices, {} color targets "
        "(mask 0x{:X}), textures vs {} ps {}",
        vertex_host_shader->pack_key,
        pixel_host_shader ? pixel_host_shader->pack_key : std::string("none"),
        uint32_t(primitive_processing_result.host_primitive_type), uint32_t(dk_primitive),
        primitive_processing_result.host_draw_vertex_count, color_target_count,
        normalized_color_mask, vertex_host_shader->texture_bindings.size(),
        pixel_host_shader ? pixel_host_shader->texture_bindings.size() : 0);
  }
  return true;
}

bool Deko3DCommandProcessor::IssueCopy() {
  static bool logged_entry = false;
  if (!logged_entry) {
    logged_entry = true;
    REXGPU_INFO("deko3d: first resolve draw/copy");
  }
  if (!render_target_cache_) {
    return true;
  }
  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(*register_file_, *memory_, 1, 1, false, false,
                                 resolve_info)) {
    static bool logged_invalid = false;
    if (!logged_invalid) {
      logged_invalid = true;
      REXGPU_WARN("deko3d: invalid resolve info; dropping the copy/clear");
    }
    return true;
  }
  return render_target_cache_->Resolve(resolve_info);
}

void Deko3DCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
#if REX_PLATFORM_SWITCH
  // The profiler's guest frame counter (the Vulkan path counts here too).
  RexSwitchPerfCount(0);
#endif
  ReportFrameDrawStats();
  ++frame_index_;
  // Submit this frame's clears, draws and resolves before the presenter copies
  // the front buffer to the swapchain.
  if (render_target_cache_) {
    render_target_cache_->Flush();
  }
  DkFence* frame_fence =
      render_target_cache_ ? render_target_cache_->frame_fence() : nullptr;
  if (!graphics_system_) {
    return;
  }
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    return;
  }
  const uint32_t display_width = frontbuffer_width ? frontbuffer_width : 1280;
  const uint32_t display_height = frontbuffer_height ? frontbuffer_height : 720;
  dk::Image target_image{};
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  if (render_target_cache_ && frontbuffer_ptr) {
    if (Deko3DRenderTargetCache::Target* target =
            render_target_cache_->target_for_resolved_address(frontbuffer_ptr)) {
      target_image = target->image();
      target_width = target->width();
      target_height = target->height();
    }
  }
  // Never CPU-read the guest front buffer for presentation; the GPU resolve
  // wrote the mirror, and the committed-page check still faults on this
  // title's non-CPU-mapped swap allocation.
  const uint8_t* guest_pixels = nullptr;
  const uint32_t guest_pitch = display_width * 4;
  DkGpuAddr guest_gpu_pixels = 0;
  uint32_t guest_physical = 0;
  if (shared_memory_ && render_target_cache_ &&
      render_target_cache_->HasResolvedFrontBuffer(frontbuffer_ptr) && frontbuffer_ptr) {
    // Fable's swap pointer (and RB_COPY_DEST_BASE) is a guest *physical*
    // address in the 512 MB GPU range; GetPhysicalAddress would treat it as a
    // virtual address and fail. Use it directly when it fits, and only try a
    // virtual-to-physical translation otherwise.
    uint64_t offset = frontbuffer_ptr;
    const uint64_t byte_size = uint64_t(display_width) * display_height * 4;
    if (offset + byte_size > SharedMemory::kBufferSize) {
      guest_physical = memory_->GetPhysicalAddress(frontbuffer_ptr);
      offset = guest_physical;
    }
    if (offset + byte_size <= SharedMemory::kBufferSize) {
      guest_gpu_pixels = shared_memory_->gpu_address() + offset;
    }
  }
  static bool logged_swap = false;
  if (!logged_swap) {
    logged_swap = true;
    REXGPU_WARN(
        "deko3d: first swap: frontbuffer 0x{:08X} ({}x{}), physical 0x{:08X}, "
        "gpu 0x{:016X}, shared {}",
        frontbuffer_ptr, display_width, display_height, guest_physical,
        uint64_t(guest_gpu_pixels), shared_memory_ != nullptr);
  }
  const bool refreshed = presenter->RefreshGuestOutput(
      display_width, display_height, display_width, display_height,
      [guest_pixels, guest_gpu_pixels, target_image, target_width, target_height,
       display_width, display_height, guest_pitch, frame_fence](
          ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& deko3d_context =
            static_cast<ui::deko3d::Deko3DGuestOutputRefreshContext&>(context);
        if (frame_fence) {
          deko3d_context.SetRenderFence(frame_fence);
        }
        if (target_width) {
          deko3d_context.SetGuestOutputTarget(target_image, target_width, target_height,
                                              display_width, display_height);
        } else if (guest_gpu_pixels) {
          deko3d_context.SetGuestOutputGpu(guest_gpu_pixels, display_width, display_height,
                                           guest_pitch);
        } else {
          deko3d_context.SetGuestOutput(guest_pixels, display_width, display_height,
                                        guest_pitch);
        }
        context.SetIs8bpc(true);
        return target_width != 0 || guest_gpu_pixels != 0 || guest_pixels != nullptr;
      });
  (void)refreshed;
}

}  // namespace rex::graphics::deko3d
