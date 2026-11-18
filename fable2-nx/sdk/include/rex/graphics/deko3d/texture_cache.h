/**
 * @file        graphics/deko3d/texture_cache.h
 * @brief       Guest textures as deko3d images.
 *
 * D2 brings up image creation and CPU untiling/upload for the formats the menu
 * uses (RGBA8 and BCn). Descriptor binding and sampling are wired in D3 with
 * the DKSH pipeline cache.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>
#include <memory>

#include <deko3d.hpp>

#include <rex/graphics/deko3d/fence_wait.h>
#include <rex/graphics/pipeline/texture/cache.h>

namespace rex::ui::deko3d {
class Deko3DDevice;
}

namespace rex::graphics::deko3d {

class Deko3DSharedMemory;

class Deko3DTextureCache final : public TextureCache {
 public:
  Deko3DTextureCache(const RegisterFile& register_file, Deko3DSharedMemory& shared_memory,
                     ui::deko3d::Deko3DDevice& device);
  ~Deko3DTextureCache() override;

  Deko3DTextureCache(const Deko3DTextureCache&) = delete;
  Deko3DTextureCache& operator=(const Deko3DTextureCache&) = delete;

  bool Initialize();
  void Shutdown();

  // Run-066: the command processor hands us its current frame list, so texture
  // copies are recorded into the frame and submitted with the frame fence - the
  // one submit path proven to execute. Returning nullptr keeps the old
  // immediate-submit fallback.
  void set_frame_recorder(std::function<dk::CmdBuf*()> recorder) {
    frame_recorder_ = std::move(recorder);
  }

  // Run-077: submit the accumulated copy-only upload prologue list immediately
  // before the frame's draw list (called from IssueSwap). No-op when empty.
  void SubmitUploadPrologue();

  // What a draw binds for one texture fetch constant.
  struct BoundTexture {
    dk::Image* image = nullptr;
    DkImageType image_type = DkImageType_2D;
    xenos::DataDimension dimension = xenos::DataDimension::k2DOrStacked;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth_or_array_size = 1;
  };

  // The host image for a fetch constant, valid after RequestTextures included
  // it. want_signed selects the signed version of a format that stores signed
  // and unsigned data separately. Returns false when the fetch constant is
  // invalid or the texture could not be created or loaded, in which case the
  // draw binds a placeholder instead.
  bool GetBoundTexture(uint32_t fetch_constant, bool want_signed, BoundTexture& out);

 protected:
  uint32_t GetHostFormatSwizzle(TextureKey key) const override {
    (void)key;
    return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override {
    (void)dimension;
    return 8192;
  }
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override {
    (void)dimension;
    return 2048;
  }
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  class TextureImpl;

  // Run-056 one-shot upload self-test (cvar deko3d_upload_probe). Submits one
  // small copyBufferToImage per variant on the shared queue at boot and logs
  // which ones complete. Returns true when every variant it attempted did.
  bool RunUploadProbe();

  // Run-061: one 64x64 copy there and back, byte compared (cvar
  // deko3d_upload_validate).
  bool RunCopyDiagnostic();
  bool RunCopyBisect();
  bool RunCopySyncTest();
  bool RunCopyMatrix();

  ui::deko3d::Deko3DDevice& device_;
  dk::UniqueCmdBuf upload_command_buffer_;
  dk::UniqueMemBlock upload_command_memory_;
  // Used instead of dkQueueWaitIdle for the upload: waiting for the whole queue
  // also waits for anything already submitted on it, and an unbounded wait turns
  // a stalled list into a hung command processor thread.
  dk::Fence upload_fence_{};
  FenceWaitStats upload_fence_stats_;
  bool upload_stopped_ = false;
  std::function<dk::CmdBuf*()> frame_recorder_;
  // Run-077: the copy-only prologue list accumulates uploads during frame
  // recording and is submitted (flushed) right before the draw list.
  bool prologue_open_ = false;
  uint32_t prologue_copies_ = 0;
  dk::Fence prologue_fence_{};
  // Set by RunUploadValidate (step 0): whether a fence signalled in a clean
  // process. Retirement prefers fences when they work and falls back to age.
  bool fences_signalled_ = false;
};

}  // namespace rex::graphics::deko3d
