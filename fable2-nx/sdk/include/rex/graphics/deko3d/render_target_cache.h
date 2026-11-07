/**
 * @file        graphics/deko3d/render_target_cache.h
 * @brief       deko3d host render targets and resolve-to-guest-memory path.
 *
 * The Vulkan backend emulates EDRAM with per-tile ownership transfers and
 * resolve compute shaders, which cost ~87% of the Switch frame at the menu.
 * This cache keeps one deko3d image per EDRAM render-target key instead and
 * resolves compatible (linear 8_8_8_8) surfaces straight into the shared
 * memory mirror with a GPU copy. Ownership transfers are intentionally not
 * performed; the menu does not rely on aliasing between live targets, and the
 * simpler model is the point of the native backend.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <deko3d.hpp>

#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/util/draw.h>

namespace rex::ui::deko3d {
class Deko3DDevice;
}

namespace rex::graphics::deko3d {

class Deko3DSharedMemory;

class Deko3DRenderTargetCache final : public RenderTargetCache {
 public:
  Deko3DRenderTargetCache(const RegisterFile& register_file, const memory::Memory& memory,
                          ui::deko3d::Deko3DDevice& device,
                          Deko3DSharedMemory& shared_memory);
  ~Deko3DRenderTargetCache() override;

  Deko3DRenderTargetCache(const Deko3DRenderTargetCache&) = delete;
  Deko3DRenderTargetCache& operator=(const Deko3DRenderTargetCache&) = delete;

  bool Initialize();
  void Shutdown();

  Path GetPath() const override { return Path::kHostRenderTargets; }

  // Records the resolve / clear described by resolve_info. Returns false only
  // for a hard failure (the guest ring should keep advancing regardless).
  bool Resolve(const draw_util::ResolveInfo& resolve_info);

  // Submits everything recorded since the previous flush and waits for the GPU
  // so the shared-memory mirror is coherent for the presenter.
  bool Flush();

  // The image currently bound for a draw (D3 will use these).
  class Target;

 protected:
  uint32_t GetMaxRenderTargetWidth() const override { return 8192; }
  uint32_t GetMaxRenderTargetHeight() const override { return 8192; }
  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;
  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override {
    (void)format;
    return false;
  }
  bool IsGammaFormatHostStorageSeparate() const override { return false; }

 private:
  Target* GetOrCreateTarget(RenderTargetKey key);
  static bool MakeResolveKey(const draw_util::ResolveEdramInfo& edram_info,
                             RenderTargetKey& key_out);
  uint32_t GetTargetHeight(RenderTargetKey key) const {
    return GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples);
  }

  bool EnsureRecording();
  void RecordColorClear(Target& target, const draw_util::ResolveInfo& resolve_info,
                        uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void RecordDepthClear(Target& target, const draw_util::ResolveInfo& resolve_info,
                        uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  bool RecordResolveCopy(Target& target, const draw_util::ResolveInfo& resolve_info,
                         uint32_t x, uint32_t y, uint32_t width, uint32_t height);

  ui::deko3d::Deko3DDevice& device_;
  Deko3DSharedMemory& shared_memory_;

  dk::UniqueCmdBuf command_buffer_;
  dk::UniqueMemBlock command_memory_;
  dk::Fence resolve_fence_{};
  bool resolve_pending_ = false;
  bool recording_ = false;
  bool has_commands_ = false;

  std::unordered_map<RenderTargetKey, std::unique_ptr<Target>, RenderTargetKey::Hasher>
      targets_;

  std::vector<std::pair<uint32_t, uint32_t>> gpu_written_ranges_;
};

// A deko3d image for one EDRAM render-target key. Public only so the command
// processor can bind it when draws land in D3.
class Deko3DRenderTargetCache::Target final : public RenderTarget {
 public:
  Target(Deko3DRenderTargetCache& cache, RenderTargetKey key) : RenderTarget(key), cache_(cache) {}

  bool Initialize();
  bool valid() const { return static_cast<bool>(memory_); }

  dk::Image& image() { return image_; }
  const dk::Image& image() const { return image_; }
  DkImageFormat format() const { return format_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t msaa_samples() const { return msaa_samples_; }

  // Returns the 1x image to copy from, resolving multisample targets first.
  dk::Image* GetResolveSource(dk::CmdBuf& command_buffer);

 private:
  Deko3DRenderTargetCache& cache_;
  dk::UniqueMemBlock memory_;
  dk::Image image_;
  dk::ImageLayout layout_;
  DkImageFormat format_ = DkImageFormat_RGBA8_Unorm;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t msaa_samples_ = 1;
  dk::UniqueMemBlock resolve_memory_;
  dk::Image resolve_image_;
};

}  // namespace rex::graphics::deko3d
