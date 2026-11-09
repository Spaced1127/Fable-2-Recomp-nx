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
#include <functional>
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
  class Target;
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

  // Whether a color resolve has written this guest address (the swap front
  // buffer). The presenter must not read the mirror before that.
  bool HasResolvedFrontBuffer(uint32_t address) const {
    return has_color_resolve_ && last_color_resolve_address_ == address;
  }

  // The host color target that resolved to this guest address, for direct
  // presentation while the guest-buffer resolve is deferred.
  Target* target_for_resolved_address(uint32_t address) const {
    auto it = resolved_targets_.find(address);
    return it != resolved_targets_.end() ? it->second : nullptr;
  }

  // Submits everything recorded since the previous flush and waits for the GPU
  // so the shared-memory mirror is coherent for the presenter.
  bool Flush();

  // Fence that the presentation queue must wait on before reading the target
  // image. Null until a render submission has been made.
  DkFence* frame_fence() { return resolve_pending_ ? &resolve_fence_ : nullptr; }

  // ---- Draw recording (D3) -------------------------------------------------
  //
  // All of a frame's GPU work - clears, draws and resolves - records into one
  // command list submitted at the swap, because this deko3d only executes one
  // graphics queue reliably and the present copy must be ordered after the
  // draws. The command processor records its draws into the same list.

  // Opens the frame's command list if needed and returns it, or nullptr on
  // failure. Opening a new list waits for the previous frame's fence, so
  // anything recorded since is complete; that is also the only safe point to
  // recycle the per-frame rings, so recording_reset_callback_ runs there.
  dk::CmdBuf* BeginRecording();

  // Marks that commands were recorded, so Flush() submits the list even if no
  // clear or resolve happened.
  void NoteCommandsRecorded() { has_commands_ = true; }

  // Called after every draw: the command memory is finite, so a long frame is
  // split into several lists rather than overflowing it.
  void CountDrawRecorded();

  // Runs whenever a new command list is opened (after the previous one's fence
  // has been waited on). The command processor uses it to reset its per-frame
  // constant, index and descriptor rings.
  void set_recording_reset_callback(std::function<void()> callback) {
    recording_reset_callback_ = std::move(callback);
  }

  // The color and depth targets the last successful Update() bound, for
  // attaching them to the draw's command list. Index 0 is depth, 1...4 are the
  // color render targets; nullptr where nothing is bound.
  Target* GetUsedRenderTarget(uint32_t index) const;

  // Number of EDRAM tile-ownership transfer ranges the last Update() asked for.
  // This backend does not copy between render targets, so a nonzero count means
  // aliased surfaces may show stale contents.
  uint32_t GetPendingTransferCount() const;

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
  // Finishes and submits the current list, waiting for it so the command
  // memory and the per-frame rings can be reused. Used mid-frame when a list
  // grows too large; Flush() is the non-blocking end-of-frame version.
  void SubmitAndWaitForReuse();
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
  dk::Fence reuse_fence_{};
  bool resolve_pending_ = false;
  bool recording_ = false;
  bool has_commands_ = false;
  uint32_t draws_since_submit_ = 0;
  std::function<void()> recording_reset_callback_;

  std::unordered_map<RenderTargetKey, std::unique_ptr<Target>, RenderTargetKey::Hasher>
      targets_;

  std::vector<std::pair<uint32_t, uint32_t>> gpu_written_ranges_;
  std::unordered_map<uint32_t, Target*> resolved_targets_;
  uint32_t last_color_resolve_address_ = 0;
  bool has_color_resolve_ = false;
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
