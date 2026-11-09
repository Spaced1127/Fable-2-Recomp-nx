/**
 * @file        graphics/deko3d/command_processor.h
 * @brief       CommandProcessor for the deko3d backend.
 *
 * Draws are issued by binding the guest's pre-compiled DKSH shaders and the
 * Xenos state directly to a deko3d command list: this deko3d has no
 * pipeline-state-object API, so shaders, rasterizer, blend, depth/stencil,
 * multisample, viewport and scissor state are all bound dynamically per draw.
 * There are no host vertex buffers either - the translated vertex shaders
 * fetch their vertices from the shared memory mirror using gl_VertexID and the
 * fetch constants.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <deko3d.hpp>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/deko3d/ring_buffer.h>
#include <rex/graphics/deko3d/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/util/draw.h>
#include <rex/string/buffer.h>

namespace rex::ui::deko3d {
class Deko3DDevice;
}

namespace rex::graphics::deko3d {

class Deko3DPrimitiveProcessor;
class Deko3DRenderTargetCache;
class Deko3DShaderPack;
class Deko3DSharedMemory;
class Deko3DTextureCache;

class Deko3DCommandProcessor final : public CommandProcessor {
 public:
  Deko3DCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state,
                         ui::deko3d::Deko3DDevice* device);
  ~Deko3DCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  // One draw's worth of constant buffers, in the storage/uniform buffer slots
  // the converted GLSL declares: SSBO 0 system constants, SSBO 1 shared
  // memory, UBO 1/2 vertex/pixel float constants, UBO 3 bool and loop
  // constants, UBO 4 fetch constants.
  struct ConstantBuffers {
    DkGpuAddr system_constants = 0;
    DkGpuAddr float_vertex = 0;
    uint32_t float_vertex_size = 0;
    DkGpuAddr float_pixel = 0;
    uint32_t float_pixel_size = 0;
    DkGpuAddr bool_loop = 0;
    DkGpuAddr fetch = 0;
  };

  // Recycles the per-frame rings. Called by the render target cache when it
  // opens a new command list, which is the one point where the previous list
  // is known to have completed.
  void ResetPerFrameRings();
  bool InitializePlaceholderTexture();

  void UpdateSystemConstantValues(bool primitive_polygonal,
                                  const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);
  bool UploadConstantBuffers(const Deko3DShader& vertex_shader, const Deko3DShader* pixel_shader,
                             ConstantBuffers& buffers_out);
  bool BindTextures(dk::CmdBuf& command_buffer, const Deko3DShader::HostShader& vertex_host_shader,
                    const Deko3DShader::HostShader* pixel_host_shader);
  bool BindRenderTargets(dk::CmdBuf& command_buffer, uint32_t normalized_color_mask,
                         uint32_t& color_target_count_out);
  void BindPipelineState(dk::CmdBuf& command_buffer, bool primitive_polygonal,
                         reg::RB_DEPTHCONTROL normalized_depth_control,
                         uint32_t normalized_color_mask, uint32_t color_target_count,
                         const draw_util::ViewportInfo& viewport_info,
                         const PrimitiveProcessor::ProcessingResult& primitive_processing_result);

  // Reports a skipped draw once per reason and counts it; always returns true
  // so the guest ring keeps advancing.
  bool DrawSkipped(const char* reason);
  void ReportFrameDrawStats();

  uint64_t GetVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, bool ps_param_gen_used) const;
  uint64_t GetPixelShaderModification(const Shader& shader, uint32_t interpolator_mask,
                                      uint32_t param_gen_pos,
                                      reg::RB_DEPTHCONTROL normalized_depth_control) const;

  ui::deko3d::Deko3DDevice* device_ = nullptr;
  std::unique_ptr<Deko3DSharedMemory> shared_memory_;
  std::unique_ptr<Deko3DRenderTargetCache> render_target_cache_;
  std::unique_ptr<Deko3DShaderPack> shader_pack_;
  std::unique_ptr<Deko3DTextureCache> texture_cache_;
  std::unique_ptr<Deko3DShaderCodeArena> shader_code_;
  std::unique_ptr<Deko3DPrimitiveProcessor> primitive_processor_;
  std::unique_ptr<Deko3DGpuRing> constant_ring_;
  std::unique_ptr<Deko3DGpuRing> descriptor_ring_;

  // Owned here, keyed by microcode hash; the base command processor only keeps
  // raw pointers to the active ones.
  std::unordered_map<uint64_t, std::unique_ptr<Deko3DShader>> shaders_;
  string::StringBuffer ucode_disasm_buffer_;

  SpirvShaderTranslator::SystemConstants system_constants_{};
  uint32_t current_float_constant_map_vertex_[4] = {};
  uint32_t current_float_constant_map_pixel_[4] = {};

  // The last uploaded contents of each constant buffer and where they live in
  // the ring. Adjacent draws usually repeat most of them, and reusing the slice
  // keeps a frame's worth of draws well inside the ring; all of it is dropped
  // when the ring is reset.
  struct CachedConstants {
    std::vector<uint8_t> bytes;
    DkGpuAddr address = 0;
    bool valid = false;
  };
  CachedConstants cached_system_constants_;
  CachedConstants cached_float_constants_vertex_;
  CachedConstants cached_float_constants_pixel_;
  CachedConstants cached_bool_loop_constants_;
  CachedConstants cached_fetch_constants_;
  bool UploadConstants(CachedConstants& cache, const void* data, size_t size,
                       DkGpuAddr& address_out);

  // Bound when a fetch constant has no valid texture, so a missing texture is
  // a white pixel rather than an uninitialized descriptor.
  dk::UniqueMemBlock placeholder_memory_;
  dk::Image placeholder_image_;
  dk::ImageLayout placeholder_layout_;

  uint32_t frame_index_ = 0;
  uint32_t frame_draws_ = 0;
  uint32_t frame_skips_ = 0;
  std::unordered_map<std::string, uint32_t> skip_reasons_;
};

}  // namespace rex::graphics::deko3d
