/**
 * @file        graphics/deko3d/render_target_cache.cpp
 * @brief       deko3d host render targets and GPU resolve path.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstring>
#include <mutex>

#include <rex/graphics/deko3d/fence_wait.h>
#include <rex/graphics/deko3d/render_target_cache.h>
#include <rex/graphics/deko3d/shared_memory.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>

namespace rex::graphics::deko3d {

namespace {

// One frame's clears, draws and resolves all record into this. Kept small
// because the process runs near its memory pool limit; kDrawsPerCommandList
// bounds how much a single list can hold instead.
constexpr uint32_t kCommandMemorySize = 2 << 20;
// Draws recorded before the list is split and submitted mid-frame. A draw with
// a full state change is on the order of a hundred 4-byte words, so this leaves
// a few times of headroom inside kCommandMemorySize.
constexpr uint32_t kDrawsPerCommandList = 3000;
constexpr uint32_t kEdramTileWidth = xenos::kEdramTileWidthSamples;

DkImageFormat GetColorImageFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return DkImageFormat_RGBA8_Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      // The guest's final menu target is 2_10_10_10, but the swap resolve is
      // 8_8_8_8. deko3d canvas copies do not convert formats, so store this
      // target as RGBA8 until the D3 resolve shader can do the conversion.
      return DkImageFormat_RGBA8_Unorm;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DkImageFormat_RG16_Unorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DkImageFormat_RG16_Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DkImageFormat_RGBA16_Unorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DkImageFormat_RGBA16_Float;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      // No 2_10_10_10 float render target on deko3d; use the closest float
      // format. The menu path uses 8_8_8_8; this is for later passes.
      return DkImageFormat_RGBA16_Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DkImageFormat_R32_Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DkImageFormat_RG32_Float;
    default:
      return DkImageFormat_RGBA8_Unorm;
  }
}

DkImageFormat GetDepthImageFormat(xenos::DepthRenderTargetFormat format) {
  return format == xenos::DepthRenderTargetFormat::kD24S8 ? DkImageFormat_Z24S8
                                                          : DkImageFormat_ZF32_X24S8;
}

DkMsMode GetMsMode(uint32_t samples) {
  switch (samples) {
    case 2:
      return DkMsMode_2x;
    case 4:
      return DkMsMode_4x;
    default:
      return DkMsMode_1x;
  }
}

// Binds the multisample state matching a target, for operations (clears) issued
// against it outside of a draw, where no pipeline state has been bound yet.
void BindTargetMultisample(dk::CmdBuf& command_buffer,
                           const Deko3DRenderTargetCache::Target& target) {
  DkMultisampleState multisample;
  dkMultisampleStateDefaults(&multisample);
  multisample.mode = GetMsMode(target.msaa_samples());
  multisample.rasterizerMode = multisample.mode;
  command_buffer.bindMultisampleState(multisample);
  command_buffer.setSampleMask(0xFFFFFFFFu);
}

}  // namespace
bool Deko3DRenderTargetCache::MakeResolveKey(
    const draw_util::ResolveEdramInfo& edram_info, RenderTargetKey& key_out) {
  key_out.key = 0;
  key_out.base_tiles = edram_info.base_tiles;
  key_out.pitch_tiles_at_32bpp =
      edram_info.pitch_tiles >> (edram_info.format_is_64bpp ? 1 : 0);
  key_out.msaa_samples = edram_info.msaa_samples;
  key_out.is_depth = edram_info.is_depth;
  key_out.resource_format = edram_info.format;
  return key_out.pitch_tiles_at_32bpp != 0;
}

Deko3DRenderTargetCache::Deko3DRenderTargetCache(
    const RegisterFile& register_file, const memory::Memory& memory,
    ui::deko3d::Deko3DDevice& device, Deko3DSharedMemory& shared_memory)
    : RenderTargetCache(register_file, memory, 1, 1),
      device_(device),
      shared_memory_(shared_memory) {}

Deko3DRenderTargetCache::~Deko3DRenderTargetCache() = default;

bool Deko3DRenderTargetCache::Initialize() {
  InitializeCommon();
  command_memory_ = dk::MemBlockMaker{device_.device(), kCommandMemorySize}
                        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                        .create();
  if (!command_memory_) {
    REXGPU_ERROR("deko3d: render target command memory allocation failed");
    return false;
  }
  command_buffer_ = dk::CmdBufMaker{device_.device()}.create();
  if (!command_buffer_) {
    REXGPU_ERROR("deko3d: render target command buffer creation failed");
    return false;
  }
  REXGPU_INFO("deko3d: render target cache initialized (host render targets)");
  return true;
}

void Deko3DRenderTargetCache::Shutdown() {
  Flush();
  targets_.clear();
  command_buffer_ = nullptr;
  command_memory_ = nullptr;
  ShutdownCommon();
}

RenderTargetCache::RenderTarget* Deko3DRenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  return GetOrCreateTarget(key);
}

Deko3DRenderTargetCache::Target* Deko3DRenderTargetCache::GetOrCreateTarget(
    RenderTargetKey key) {
  auto it = targets_.find(key);
  if (it != targets_.end()) {
    return it->second.get();
  }
  std::unique_ptr<Target> target = std::make_unique<Target>(*this, key);
  if (!target->Initialize()) {
    return nullptr;
  }
  Target* target_ptr = target.get();
  targets_.emplace(key, std::move(target));
  REXGPU_INFO("deko3d: render target {} ({}x{}, {}x MSAA)", key.GetDebugName(),
              target_ptr->width(), target_ptr->height(), target_ptr->msaa_samples());
  return target_ptr;
}

bool Deko3DRenderTargetCache::EnsureRecording() {
  if (recording_) {
    return true;
  }
  if (rendering_stopped_ || !command_buffer_ || !command_memory_) {
    return false;
  }
  // The previous list is still using the command memory. Wait for its fence -
  // bounded, because a list this queue never completes is a failure this backend
  // has already met twice, and this is the thread that drives the guest ring:
  // blocking here forever stops the game, not just the renderer.
  if (resolve_pending_) {
    if (!WaitFence(resolve_fence_, "previous frame's render list", resolve_fence_stats_)) {
      rendering_stopped_ = true;
      REXGPU_WARN(
          "deko3d: giving up on render recording; the guest ring keeps advancing and "
          "the last frame presented stays on screen");
      return false;
    }
    resolve_pending_ = false;
    for (const std::pair<uint32_t, uint32_t>& range : gpu_written_ranges_) {
      shared_memory_.RangeWrittenByGpu(range.first, range.second);
    }
    gpu_written_ranges_.clear();
  }
  static bool logged_record_start = false;
  if (!logged_record_start) {
    logged_record_start = true;
    REXGPU_INFO("deko3d: render target recording started");
  }
  command_buffer_.clear();
  command_buffer_.addMemory(command_memory_, 0, command_memory_.getSize());
  recording_ = true;
  has_commands_ = false;
  draws_since_submit_ = 0;
  // This is the one point where the GPU is known to be done with the previous
  // list, so it is also where the command processor may recycle its per-frame
  // constant, index and descriptor rings.
  if (recording_reset_callback_) {
    recording_reset_callback_();
  }
  // Drop whatever the GPU still holds cached of descriptors and shader code
  // written since the previous list. This barrier alone is not a reason to
  // submit: has_commands_ stays false until something real is recorded.
  command_buffer_.barrier(DkBarrier_None,
                          DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
  return true;
}

dk::CmdBuf* Deko3DRenderTargetCache::BeginRecording() {
  return EnsureRecording() ? &command_buffer_ : nullptr;
}

bool Deko3DRenderTargetCache::WaitForPendingList(const char* before_what) {
  if (!resolve_pending_) {
    return true;
  }
  if (!WaitFence(resolve_fence_, before_what, resolve_fence_stats_)) {
    rendering_stopped_ = true;
    REXGPU_WARN("deko3d: the render list submitted before %s never completed; render "
                "recording is stopped",
                before_what);
    return false;
  }
  resolve_pending_ = false;
  for (const std::pair<uint32_t, uint32_t>& range : gpu_written_ranges_) {
    shared_memory_.RangeWrittenByGpu(range.first, range.second);
  }
  gpu_written_ranges_.clear();
  return true;
}

void Deko3DRenderTargetCache::CountDrawRecorded() {
  if (++draws_since_submit_ < kDrawsPerCommandList) {
    return;
  }
  SubmitAndWaitForReuse();
}

void Deko3DRenderTargetCache::SubmitAndWaitForReuse() {
  if (!recording_) {
    return;
  }
  recording_ = false;
  draws_since_submit_ = 0;
  if (!has_commands_) {
    return;
  }
  has_commands_ = false;
  DkCmdList list = command_buffer_.finishList();
  {
    std::lock_guard lock(device_.submit_mutex());
    device_.queue().submitCommands(list);
    device_.queue().signalFence(reuse_fence_, true);
  }
  // Blocking on purpose: the command memory and the per-frame rings are about to
  // be written again, so the GPU must have finished reading them. Bounded for the
  // same reason as every other wait - a stall must become a log line, not a hung
  // game thread.
  if (!WaitFence(reuse_fence_, "mid-frame command list", reuse_fence_stats_)) {
    rendering_stopped_ = true;
    REXGPU_WARN("deko3d: giving up on render recording after a mid-frame list split");
    return;
  }
  for (const std::pair<uint32_t, uint32_t>& range : gpu_written_ranges_) {
    shared_memory_.RangeWrittenByGpu(range.first, range.second);
  }
  gpu_written_ranges_.clear();
  static bool logged_split = false;
  if (!logged_split) {
    logged_split = true;
    REXGPU_WARN("deko3d: frame split after {} draws; the command list grew too "
                "large for one submission",
                kDrawsPerCommandList);
  }
}

Deko3DRenderTargetCache::Target* Deko3DRenderTargetCache::GetUsedRenderTarget(
    uint32_t index) const {
  if (index > xenos::kMaxColorRenderTargets) {
    return nullptr;
  }
  return static_cast<Target*>(last_update_used_render_targets()[index]);
}

uint32_t Deko3DRenderTargetCache::GetPendingTransferCount() const {
  // last_update_transfers() points at the first of 1 + kMaxColorRenderTargets
  // vectors (index 0 is depth).
  const std::vector<Transfer>* transfers = last_update_transfers();
  uint32_t total = 0;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    total += uint32_t(transfers[i].size());
  }
  return total;
}

bool Deko3DRenderTargetCache::Flush() {
  if (!recording_) {
    return true;
  }
  recording_ = false;
  draws_since_submit_ = 0;
  if (!has_commands_) {
    return true;
  }
  has_commands_ = false;
  DkCmdList list = command_buffer_.finishList();
  {
    std::lock_guard lock(device_.submit_mutex());
    device_.queue().submitCommands(list);
    device_.queue().signalFence(resolve_fence_, true);
    resolve_pending_ = true;
  }
  static bool logged_resolve_submit = false;
  if (!logged_resolve_submit) {
    logged_resolve_submit = true;
    REXGPU_INFO("deko3d: resolve list submitted on the render queue");
  }
  // Do not wait here: the presentation queue waits for this fence before it
  // reads the target image, so render work never sits behind a present.
  return true;
}

void Deko3DRenderTargetCache::RecordColorClear(
    Target& target, const draw_util::ResolveInfo& resolve_info, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height) {
  if (!EnsureRecording()) {
    return;
  }
  static bool logged_first_clear = false;
  if (!logged_first_clear) {
    logged_first_clear = true;
    REXGPU_INFO("deko3d: first color clear: value 0x{:08X}, {}x{} at ({},{}), target {}",
                resolve_info.rb_color_clear, width, height, x, y, target.key().GetDebugName());
  }
  const uint32_t clear = resolve_info.rb_color_clear;
  float rgba[4] = {
      float(clear & 0xFF) / 255.0f,
      float((clear >> 8) & 0xFF) / 255.0f,
      float((clear >> 16) & 0xFF) / 255.0f,
      float((clear >> 24) & 0xFF) / 255.0f,
  };
  // The multisample state has to match the target being cleared: a clear of a
  // 2DMS image while the bound mode still says 1x is what a stalled 2D engine
  // looked like in the first run that reached 2x render targets.
  BindTargetMultisample(command_buffer_, target);
  dk::ImageView view{target.image()};
  command_buffer_.bindRenderTargets(&view);
  command_buffer_.setScissors(0, {{x, y, width, height}});
  command_buffer_.clearColor(0, DkColorMask_RGBA, rgba[0], rgba[1], rgba[2], rgba[3]);
  has_commands_ = true;
}

void Deko3DRenderTargetCache::RecordDepthClear(
    Target& target, const draw_util::ResolveInfo& resolve_info, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height) {
  if (!EnsureRecording()) {
    return;
  }
  // The register is the guest depth clear value; for D24S8 the useful value is
  // the 24-bit fixed-point field. Clamp to the host's 0...1 range.
  float depth = float(resolve_info.rb_depth_clear & 0xFFFFFF) / 16777215.0f;
  BindTargetMultisample(command_buffer_, target);
  dk::ImageView view{target.image()};
  command_buffer_.bindRenderTargets(nullptr, &view);
  command_buffer_.setScissors(0, {{x, y, width, height}});
  command_buffer_.clearDepthStencil(true, depth, 0, 0);
  has_commands_ = true;
}

bool Deko3DRenderTargetCache::RecordResolveCopy(
    Target& target, const draw_util::ResolveInfo& resolve_info, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height) {
  if (!EnsureRecording()) {
    return false;
  }
  if (target.format() != DkImageFormat_RGBA8_Unorm) {
    static bool logged_unsupported_source = false;
    if (!logged_unsupported_source) {
      logged_unsupported_source = true;
      REXGPU_WARN("deko3d: resolve source format {} is not RGBA8; dropping the "
                  "copy until the conversion shader lands",
                  uint32_t(target.format()));
    }
    return true;
  }
  static bool logged_first_copy = false;
  if (!logged_first_copy) {
    logged_first_copy = true;
    REXGPU_INFO(
        "deko3d: first resolve copy: dest 0x{:08X}, format {}, pitch {}, {}x{} "
        "at ({},{}), source {} ({}x{})",
        resolve_info.copy_dest_base, uint32_t(resolve_info.copy_dest_info.copy_dest_format),
        register_file().Get<reg::RB_COPY_DEST_PITCH>().copy_dest_pitch, width, height, x, y,
        target.key().GetDebugName(), target.width(), target.height());
  }
  if (resolve_info.copy_dest_info.copy_dest_format != xenos::ColorFormat::k_8_8_8_8 ||
      uint32_t(resolve_info.copy_dest_info.copy_dest_endian) !=
          uint32_t(xenos::Endian128::kNone)) {
    static bool logged_unsupported = false;
    if (!logged_unsupported) {
      logged_unsupported = true;
      REXGPU_WARN(
          "deko3d: resolve destination format/endian unsupported (format {}), "
          "dropping the copy",
          uint32_t(resolve_info.copy_dest_info.copy_dest_format));
    }
    return true;
  }

  const reg::RB_COPY_DEST_PITCH dest_pitch_reg = register_file().Get<reg::RB_COPY_DEST_PITCH>();
  const uint32_t dest_pitch = dest_pitch_reg.copy_dest_pitch;
  if (!dest_pitch) {
    return false;
  }
  const uint32_t dest_x = resolve_info.copy_dest_coordinate_info.offset_x_div_8 * 8;
  const uint32_t dest_y = resolve_info.copy_dest_coordinate_info.offset_y_div_8 * 8;
  const uint64_t dest_offset =
      uint64_t(resolve_info.copy_dest_base) + (uint64_t(dest_y) * dest_pitch + dest_x) * 4;
  const uint64_t dest_size = uint64_t(dest_pitch) * 4 * height;
  if (dest_offset + dest_size > SharedMemory::kBufferSize) {
    REXGPU_WARN("deko3d: resolve destination outside guest memory (0x{:08X}+0x{:X})",
                uint32_t(dest_offset), uint32_t(dest_size));
    return false;
  }

  dk::Image* source = target.GetResolveSource(command_buffer_);
  if (!source) {
    return false;
  }
  if (x >= target.width() || y >= target.height()) {
    return false;
  }
  const uint32_t copy_width = std::min(width, target.width() - x);
  const uint32_t copy_height = std::min(height, target.height() - y);
  if (!copy_width || !copy_height) {
    return false;
  }
  DkCopyBuf dest{};
  dest.addr = shared_memory_.gpu_address() + dest_offset;
  dest.rowLength = dest_pitch * 4;
  dest.imageHeight = dest_pitch * 4 * height;
  dk::ImageView source_view{*source};
  const DkImageRect source_rect{x, y, 0, copy_width, copy_height, 1};
  command_buffer_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
  command_buffer_.copyImageToBuffer(source_view, source_rect, dest);
  gpu_written_ranges_.emplace_back(uint32_t(dest_offset), uint32_t(dest_size));
  has_commands_ = true;
  return true;
}

bool Deko3DRenderTargetCache::Resolve(const draw_util::ResolveInfo& resolve_info) {
  const uint32_t width = resolve_info.coordinate_info.width_div_8 * 8;
  const uint32_t height = resolve_info.height_div_8 * 8;
  const uint32_t x = resolve_info.coordinate_info.edram_offset_x_div_8 * 8;
  const uint32_t y = resolve_info.coordinate_info.edram_offset_y_div_8 * 8;

  if (resolve_info.IsClearingColor()) {
    RenderTargetKey key;
    if (MakeResolveKey(resolve_info.color_edram_info, key)) {
      if (Target* target = GetOrCreateTarget(key)) {
        RecordColorClear(*target, resolve_info, x, y, width, height);
      }
    }
  }
  if (resolve_info.IsClearingDepth()) {
    RenderTargetKey key;
    if (MakeResolveKey(resolve_info.depth_edram_info, key)) {
      if (Target* target = GetOrCreateTarget(key)) {
        RecordDepthClear(*target, resolve_info, x, y, width, height);
      }
    }
  }

  if (resolve_info.copy_dest_extent_length) {
    if (resolve_info.IsCopyingDepth()) {
      static bool logged_depth_copy = false;
      if (!logged_depth_copy) {
        logged_depth_copy = true;
        REXGPU_WARN("deko3d: depth resolve copy not implemented yet; only the "
                    "depth clear is applied");
      }
      return true;
    }
    const draw_util::ResolveEdramInfo& edram_info =
        resolve_info.IsCopyingDepth() ? resolve_info.depth_edram_info
                                      : resolve_info.color_edram_info;
    RenderTargetKey key;
    if (MakeResolveKey(edram_info, key)) {
      if (Target* target = GetOrCreateTarget(key)) {
        // Present the host target directly for now. The guest-buffer resolve
        // (copyImageToBuffer) stalls this deko3d queue; it will return as a
        // compute/DKSH pass rather than a 2D-engine copy.
        resolved_targets_[resolve_info.copy_dest_base] = target;
        last_color_resolve_address_ = resolve_info.copy_dest_base;
        has_color_resolve_ = true;
        static bool logged_deferred = false;
        if (!logged_deferred) {
          logged_deferred = true;
          REXGPU_INFO("deko3d: guest-buffer resolve deferred; presenting the host "
                      "render target for 0x{:08X}",
                      resolve_info.copy_dest_base);
        }
      }
    }
  }
  return true;
}

bool Deko3DRenderTargetCache::Target::Initialize() {
  width_ = key().GetWidth();
  height_ = cache_.GetTargetHeight(key());
  if (!width_ || !height_) {
    return false;
  }
  msaa_samples_ = uint32_t(1) << uint32_t(key().msaa_samples);
  if (key().is_depth) {
    format_ = GetDepthImageFormat(key().GetDepthFormat());
  } else {
    format_ = GetColorImageFormat(key().GetColorFormat());
  }

  dk::ImageLayoutMaker maker{cache_.device_.device()};
  const bool multisampled = msaa_samples_ > 1;
  maker.setFlags(DkImageFlags_UsageRender |
                 (multisampled ? 0 : DkImageFlags_Usage2DEngine))
      .setType(multisampled ? DkImageType_2DMS : DkImageType_2D)
      .setFormat(format_)
      .setDimensions(width_, height_)
      .setMsMode(GetMsMode(msaa_samples_));
  maker.initialize(layout_);
  const uint64_t size = layout_.getSize();
  const uint32_t alignment = layout_.getAlignment();
  if (!size || !alignment) {
    return false;
  }
  const uint64_t aligned_size = (size + alignment - 1) & ~uint64_t(alignment - 1);
  memory_ = dk::MemBlockMaker{cache_.device_.device(), uint32_t(aligned_size)}
                .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                .create();
  if (!memory_) {
    REXGPU_ERROR("deko3d: render target image allocation failed ({}x{})", width_, height_);
    return false;
  }
  image_.initialize(layout_, memory_, 0);

  if (msaa_samples_ > 1) {
    dk::ImageLayout resolve_layout;
    dk::ImageLayoutMaker{cache_.device_.device()}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
        .setFormat(format_)
        .setDimensions(width_, height_)
        .setMsMode(DkMsMode_1x)
        .initialize(resolve_layout);
    const uint64_t resolve_size = resolve_layout.getSize();
    const uint32_t resolve_alignment = resolve_layout.getAlignment();
    const uint64_t resolve_aligned =
        (resolve_size + resolve_alignment - 1) & ~uint64_t(resolve_alignment - 1);
    resolve_memory_ =
        dk::MemBlockMaker{cache_.device_.device(), uint32_t(resolve_aligned)}
            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
            .create();
    if (!resolve_memory_) {
      REXGPU_ERROR("deko3d: MSAA resolve image allocation failed ({}x{})", width_,
                   height_);
      return false;
    }
    resolve_image_.initialize(resolve_layout, resolve_memory_, 0);
  } else {
    // A 2D-engine staging image for every target. Copying directly from a
    // render-target image to a guest buffer stalled the 2D engine; SVR2011's
    // working path always copies through a texture.
    dk::ImageLayout resolve_layout;
    dk::ImageLayoutMaker{cache_.device_.device()}
        .setFlags(DkImageFlags_Usage2DEngine)
        .setFormat(format_)
        .setDimensions(width_, height_)
        .initialize(resolve_layout);
    const uint64_t resolve_size = resolve_layout.getSize();
    const uint32_t resolve_alignment = resolve_layout.getAlignment();
    const uint64_t resolve_aligned =
        (resolve_size + resolve_alignment - 1) & ~uint64_t(resolve_alignment - 1);
    resolve_memory_ =
        dk::MemBlockMaker{cache_.device_.device(), uint32_t(resolve_aligned)}
            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
            .create();
    if (!resolve_memory_) {
      REXGPU_ERROR("deko3d: resolve staging image allocation failed ({}x{})", width_,
                   height_);
      return false;
    }
    resolve_image_.initialize(resolve_layout, resolve_memory_, 0);
  }
  return true;
}

dk::Image* Deko3DRenderTargetCache::Target::GetResolveSource(dk::CmdBuf& command_buffer) {
  if (msaa_samples_ > 1) {
    command_buffer.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dk::ImageView source{image_};
    dk::ImageView destination{resolve_image_};
    command_buffer.resolveImage(source, destination);
  } else {
    command_buffer.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dk::ImageView source{image_};
    dk::ImageView destination{resolve_image_};
    const DkImageRect rect{0, 0, 0, width_, height_, 1};
    command_buffer.copyImage(source, rect, destination, rect, 0);
  }
  return &resolve_image_;
}

}  // namespace rex::graphics::deko3d
