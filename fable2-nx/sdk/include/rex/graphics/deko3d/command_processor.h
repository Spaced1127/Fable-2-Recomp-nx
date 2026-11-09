/**
 * @file        graphics/deko3d/command_processor.h
 * @brief       CommandProcessor for the deko3d backend: guest ring submission
 *              with deko3d presentation. Draws are not executed yet (bring-up).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>

#include <rex/graphics/command_processor.h>

namespace rex::ui::deko3d {
class Deko3DDevice;
}

namespace rex::graphics::deko3d {

class Deko3DSharedMemory;
class Deko3DRenderTargetCache;
class Deko3DShaderPack;
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
  ui::deko3d::Deko3DDevice* device_ = nullptr;
  std::unique_ptr<Deko3DSharedMemory> shared_memory_;
  std::unique_ptr<Deko3DRenderTargetCache> render_target_cache_;
  std::unique_ptr<Deko3DShaderPack> shader_pack_;
  std::unique_ptr<Deko3DTextureCache> texture_cache_;
};

}  // namespace rex::graphics::deko3d
