/**
 * @file        graphics/deko3d/texture_cache.cpp
 * @brief       Guest textures as deko3d images (D2 bring-up).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <atomic>
#include <thread>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/graphics/deko3d/ring_buffer.h>
#include <rex/graphics/deko3d/shared_memory.h>
#include <rex/graphics/deko3d/texture_cache.h>
#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>

REXCVAR_DEFINE_BOOL(deko3d_texture_upload, false, "GPU/deko3d",
                    "Upload guest textures with the 2D engine. Run 068 proved the copy EXECUTES "
                    "and lands the pattern (descriptor A: rowLength bytes, imageHeight rows, "
                    "Usage2DEngine tiled); the old 'stalls the queue permanently' claim was the "
                    "missing-kickoff artefact and is retracted. Off by default in code; the "
                    "staged fable_2.toml pins it true for the F1 runs.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Historical probes (RunUploadProbe / RunCopyDiagnostic / RunCopyBisect) are
// retired as verdict sources: their "fence never signals" / "copy fails"
// results were the missing-kickoff artefact (run 068 correction - see
// fence_wait.h). They remain opt-in for A/B only. The live proof is the
// frame-path upload probe (presenter-polled) and the guest-output readback.
REXCVAR_DEFINE_BOOL(deko3d_upload_validate, false, "GPU/deko3d",
                    "Retired run-065 a-d standalone copy diagnostic (its FAIL verdicts were the "
                    "missing-kickoff artefact, corrected by run 068). Off by default; the "
                    "frame-path upload probe is unconditional and presenter-polled instead.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(deko3d_upload_probe, false, "GPU/deko3d",
                    "Retired boot self-test (fence-only/clear-only controls + five copy shapes). "
                    "Its run-058 'fence primitive dead' verdict was the missing-kickoff artefact "
                    "(run 068 correction). Off by default; opt-in A/B only.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Upload protocol: submit (flushed), never wait, retire staging by fence or
// present-age. Queue order guarantees the copy precedes every draw that samples
// the image; only the staging lifetime needs the fence. (The run-057/064
// premises - "the copy's fence never signals", "CpuCached staging hangs the 2D
// engine" - are retracted by run 068: the copies execute and land the pattern
// once kicked; uncached staging is kept because it needs no CPU flush.)
// Run-067: the copy-vs-sample bisect, run at command-processor setup - before the
// first frame - with one REAL upload's parameters (960x720, xenos format 23 ->
// RGBA8, tiled, as upload #1 in run 066). It answers "does the copy alone kill the
// process, and does the destination's pitch/tiling matter?" without a guest.
REXCVAR_DEFINE_BOOL(deko3d_copy_sync, true, "GPU/deko3d",
                    "At setup, submit one 960x720 buffer->image copy, log the fence state before "
                    "and after a bounded dkQueueWaitIdle, read 64x64 back, and print "
                    "EXECUTED/DROPPED/WEDGED. Descriptor A (rows) only by default.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Run-068: descriptors B (imageHeight = total bytes - the run-060 OS-kill DMA if
// it executes) and C (tight 0/0) are opt-in. A alone decides the verdict for the
// descriptor the port uses; one measured variable per hardware run.
REXCVAR_DEFINE_BOOL(deko3d_copy_sync_extra, false, "GPU/deko3d",
                    "Also run copy-sync descriptors B (bytes) and C (tight 0/0) after A. B can "
                    "kill the process if it executes (run 060); leave off unless A was DROPPED.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(deko3d_copy_bisect, false, "GPU/deko3d",
                    "At setup, copy a 960x720 pattern into a real texture image (tiled, then "
                    "pitch-linear) with its own fence and bounded waits, reading 64x64 back. "
                    "Diagnostic: names whether the copy alone is the killer.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(deko3d_upload_staging_uncached, true, "GPU/deko3d",
                    "Use a CPU-uncached staging block as the source of the 2D-engine upload copy "
                    "(writes go straight to DRAM, no flush needed; the run-064 'CpuCached hangs "
                    "the copy' hypothesis is superseded by the run-068 kickoff finding). Set "
                    "false to A/B cached staging + flush.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(deko3d_upload_no_wait, true, "GPU/deko3d",
                    "Upload textures without waiting for the copy fence (presenter-style); the "
                    "submit is flushed so the fence signals and retires staging. Set false for "
                    "a bounded wait per texture (fences signal in ~5 ms once kicked - run 068).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::deko3d {

namespace {

constexpr uint32_t kUploadCommandSize = 64 << 10;

// Run-069: the frame-path upload probe. The readback rides the FIRST frame-list
// upload (recorded after its copy, same list), and the PRESENTER polls it every
// present - run 068 armed it but death came 240 ms later, before any later
// upload or texture-binding draw re-checked it, so the pixels never printed.
// The block is CpuUncached: with CpuCached the CPU reads its own dirty 0xAA
// pre-fill lines even when the GPU wrote the image (the run-068 lesson).
struct FrameProbe {
  dk::UniqueMemBlock block;
  bool armed = false;
  bool done = false;
  uint64_t present_epoch = 0;
};
FrameProbe g_frame_probe;

}  // namespace

// Called by the presenter on every present: prints the frame-path upload's
// readback once two presents have passed the frame that carried it (the queue
// is in-order and the present kick has executed it by then). Non-blocking.
void PollFrameUploadProbe() {
  if (!g_frame_probe.armed || g_frame_probe.done || !g_frame_probe.block) {
    return;
  }
  if (PresentEpoch() < g_frame_probe.present_epoch + 2) {
    return;
  }
  g_frame_probe.done = true;
  auto* bytes = static_cast<uint8_t*>(g_frame_probe.block.getCpuAddr());
  uint32_t changed = 0;
  for (uint32_t i = 0; i < 4096 * 8; ++i) {
    if (bytes[i] != 0xAA) {
      ++changed;
    }
  }
  REXGPU_WARN("[upload] frame-path readback (presenter poll): {} of the first 32768 bytes "
              "differ from the 0xAA pre-fill; first bytes [{:02X}{:02X}{:02X}{:02X} "
              "{:02X}{:02X}{:02X}{:02X}] - {}",
              changed, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
              bytes[7],
              changed ? "the frame-recorded upload LANDED pixels"
                      : "still the pre-fill - the frame-recorded copy produced nothing");
}

namespace {

bool GetDekoImageFormat(xenos::TextureFormat format, DkImageFormat& format_out) {
  switch (GetBaseFormat(format)) {
    case xenos::TextureFormat::k_8:
      format_out = DkImageFormat_R8_Unorm;
      return true;
    case xenos::TextureFormat::k_8_8:
      format_out = DkImageFormat_RG8_Unorm;
      return true;
    case xenos::TextureFormat::k_8_8_8_8:
      format_out = DkImageFormat_RGBA8_Unorm;
      return true;
    case xenos::TextureFormat::k_4_4_4_4:
      format_out = DkImageFormat_RGBA4_Unorm;
      return true;
    case xenos::TextureFormat::k_1_5_5_5:
      format_out = DkImageFormat_RGB5A1_Unorm;
      return true;
    case xenos::TextureFormat::k_5_6_5:
      format_out = DkImageFormat_RGB565_Unorm;
      return true;
    case xenos::TextureFormat::k_2_10_10_10:
      format_out = DkImageFormat_RGB10A2_Unorm;
      return true;
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
      format_out = DkImageFormat_RG11B10_Float;
      return true;
    case xenos::TextureFormat::k_16:
      format_out = DkImageFormat_R16_Unorm;
      return true;
    case xenos::TextureFormat::k_16_16:
      format_out = DkImageFormat_RG16_Unorm;
      return true;
    case xenos::TextureFormat::k_16_16_16_16:
      format_out = DkImageFormat_RGBA16_Unorm;
      return true;
    case xenos::TextureFormat::k_16_FLOAT:
      format_out = DkImageFormat_R16_Float;
      return true;
    case xenos::TextureFormat::k_16_16_FLOAT:
      format_out = DkImageFormat_RG16_Float;
      return true;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      format_out = DkImageFormat_RGBA16_Float;
      return true;
    case xenos::TextureFormat::k_32:
      format_out = DkImageFormat_R32_Float;
      return true;
    case xenos::TextureFormat::k_32_32:
      format_out = DkImageFormat_RG32_Float;
      return true;
    case xenos::TextureFormat::k_32_32_32_32:
      format_out = DkImageFormat_RGBA32_Float;
      return true;
    case xenos::TextureFormat::k_32_FLOAT:
      format_out = DkImageFormat_R32_Float;
      return true;
    case xenos::TextureFormat::k_32_32_FLOAT:
      format_out = DkImageFormat_RG32_Float;
      return true;
    case xenos::TextureFormat::k_32_32_32_FLOAT:
      format_out = DkImageFormat_RGB32_Float;
      return true;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      format_out = DkImageFormat_RGBA32_Float;
      return true;
    case xenos::TextureFormat::k_DXT1:
      format_out = DkImageFormat_RGBA_BC1;
      return true;
    case xenos::TextureFormat::k_DXT2_3:
      format_out = DkImageFormat_RGBA_BC2;
      return true;
    case xenos::TextureFormat::k_DXT4_5:
      format_out = DkImageFormat_RGBA_BC3;
      return true;
    case xenos::TextureFormat::k_DXN:
      format_out = DkImageFormat_RG_BC5_Unorm;
      return true;
    case xenos::TextureFormat::k_24_8:
      format_out = DkImageFormat_Z24S8;
      return true;
    case xenos::TextureFormat::k_24_8_FLOAT:
      format_out = DkImageFormat_ZF32_X24S8;
      return true;
    default:
      return false;
  }
}

}  // namespace

class Deko3DTextureCache::TextureImpl final : public Texture {
 public:
  TextureImpl(Deko3DTextureCache& texture_cache, TextureKey key)
      : Texture(texture_cache, key), texture_cache_(texture_cache) {}

  bool Initialize(DkImageFormat format, DkImageType type, uint32_t width, uint32_t height,
                  uint32_t depth) {
    width_ = width;
    height_ = height;
    depth_ = depth;
    format_ = format;
    image_type_ = type;

    dk::ImageLayoutMaker maker{texture_cache_.device_.device()};
    maker.setFlags(DkImageFlags_Usage2DEngine)
        .setType(type)
        .setFormat(format)
        .setDimensions(width, height, depth)
        .setMipLevels(1);
    maker.initialize(layout_);
    const uint64_t size = layout_.getSize();
    const uint32_t alignment = layout_.getAlignment();
    if (!size || !alignment) {
      return false;
    }
    const uint64_t aligned = (size + alignment - 1) & ~uint64_t(alignment - 1);
    memory_ = dk::MemBlockMaker{texture_cache_.device_.device(), uint32_t(aligned)}
                  .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                  .create();
    if (!memory_) {
      REXGPU_ERROR("deko3d: texture image allocation failed ({}x{})", width, height);
      return false;
    }
    image_.initialize(layout_, memory_, 0);
    SetHostMemoryUsage(aligned);
    return true;
  }

  dk::Image& image() { return image_; }
  DkImageFormat format() const { return format_; }
  DkImageType image_type() const { return image_type_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t depth() const { return depth_; }
  bool valid() const { return static_cast<bool>(memory_); }

 private:
  Deko3DTextureCache& texture_cache_;
  dk::UniqueMemBlock memory_;
  dk::Image image_;
  dk::ImageLayout layout_;
  DkImageFormat format_ = DkImageFormat_RGBA8_Unorm;
  DkImageType image_type_ = DkImageType_2D;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t depth_ = 1;
};

Deko3DTextureCache::Deko3DTextureCache(const RegisterFile& register_file,
                                       Deko3DSharedMemory& shared_memory,
                                       ui::deko3d::Deko3DDevice& device)
    : TextureCache(register_file, shared_memory, 1, 1), device_(device) {}

Deko3DTextureCache::~Deko3DTextureCache() = default;

bool Deko3DTextureCache::Initialize() {
  upload_command_memory_ =
      dk::MemBlockMaker{device_.device(), kUploadCommandSize}
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
  if (!upload_command_memory_) {
    REXGPU_ERROR("deko3d: texture upload command memory allocation failed");
    return false;
  }
  upload_command_buffer_ = dk::CmdBufMaker{device_.device()}.create();
  if (!upload_command_buffer_) {
    REXGPU_ERROR("deko3d: texture upload command buffer creation failed");
    return false;
  }
  // Run-062: NOTHING here waits. Run 061 called a validation that ended in
  // dkQueueWaitIdle and the queue never completed the list, so boot stopped dead
  // before the first frame - the exact failure fence_wait.h warns about. The
  // validation now lives in the upload path (below) and is read back by age.
  if (REXCVAR_GET(deko3d_copy_sync)) {
    RunCopySyncTest();
  }
  if (REXCVAR_GET(deko3d_copy_bisect)) {
    RunCopyBisect();
  } else {
    REXGPU_WARN("[copy-bisect] gate: deko3d_copy_bisect=false -> skipped");
  }
  {
    const bool want_validate = REXCVAR_GET(deko3d_upload_validate);
    REXGPU_WARN("[upload-validate] gate: deko3d_upload_validate={} -> {}; queue handle 0x{:X} "
                "(the only queue: present, upload and validate share it)",
                want_validate ? "true" : "false",
                want_validate ? "armed for the frame loop (never blocks)"
                              : "off (opt-in diagnostic)",
                uint64_t(device_.queue()));
  }
  if (REXCVAR_GET(deko3d_upload_probe)) {
    REXGPU_WARN("[upload-probe] gate: cvar deko3d_upload_probe={} -> running the self-test",
                "true");
    RunUploadProbe();
  } else {
    // Logged even when off: run 056 produced no probe lines and the log could
    // not say whether the flag was read, so the gate always names itself now.
    REXGPU_WARN("[upload-probe] gate: cvar deko3d_upload_probe={} -> self-test skipped",
                "false");
  }
  REXGPU_INFO("deko3d: texture cache initialized");
  return true;
}

void Deko3DTextureCache::Shutdown() {
  DestroyAllTextures(true);
  upload_command_buffer_ = nullptr;
  upload_command_memory_ = nullptr;
}

// Run-056: the upload self-test. RETIRED as a verdict source (run 068): the
// "fence never signals / queue wedged" premise below was the missing-kickoff
// artefact plus the run-060 imageHeight-units bug, both corrected. Kept opt-in
// for A/B only; the shapes it measures are listed for reference:
//
//   1 control-presenter-flags  the presenter's own image creation (render +
//                              present + 2D engine + hw compression) - the only
//                              image this port already copies into
//   2 baseline-2dengine        exactly today's failing path, 64x64
//   3 staging-cpuuncached      the staging block with an uncached CPU mapping
//   4 dest-pitchlinear         the destination as a pitch-linear (untiled) image
//   5 no-inlist-barrier        the barrier after the copy removed
//
// A hanging copy wedges the queue for the rest of the session, so only the
// variants up to the first failure produce a real answer: the list is ordered
// with the most likely to complete first. One run therefore still separates
// "the copy path is unusable at all" (variant 1 fails) from "the texture image
// creation is wrong" (1 passes, 2 fails) and from each single-variable shape.
bool Deko3DTextureCache::RunUploadProbe() {
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 64;
  // Same padded source pitch as the real path: a 64-pixel RGBA8 row is 256
  // bytes, padded to 4096 like every texture upload since run 037.
  constexpr uint32_t kPitch = 4096;
  constexpr uint32_t kStagingBytes = kPitch * kHeight;
  // Short: a healthy copy is microseconds, and run 057 showed a lost fence is
  // not a stalled queue, so no variant is allowed to cost 5 s.
  constexpr int64_t kFenceNs = 1000000000;

  enum class Kind { kFenceOnly, kClearOnly, kCopy };
  struct Variant {
    const char* name;
    Kind kind;
    uint32_t image_flags;
    bool staging_uncached;
    bool barrier;
  };
  // Run-057 redesign. The old order stopped at the first timeout, which threw
  // away four data points and (worse) assumed a timed-out fence meant a dead
  // queue - the frames kept advancing, so it did not. Now:
  //   1 the fences are tested on their own: an empty list with a signal, and a
  //     clear into a small render target. If either times out, every copy
  //     result below is meaningless and the fault is dkFenceWait, not the copy;
  //   2 then the five copy shapes, each still one variable, all attempted.
  const Variant variants[] = {
      {"fence-only", Kind::kFenceOnly,
       DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine |
           DkImageFlags_HwCompression,
       false, true},
      {"clear-only", Kind::kClearOnly,
       DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression,
       false, true},
      {"control-presenter-flags", Kind::kCopy,
       DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine |
           DkImageFlags_HwCompression,
       false, true},
      {"baseline-2dengine", Kind::kCopy, DkImageFlags_Usage2DEngine, false, true},
      {"staging-cpuuncached", Kind::kCopy, DkImageFlags_Usage2DEngine, true, true},
      {"dest-pitchlinear", Kind::kCopy, DkImageFlags_Usage2DEngine | DkImageFlags_PitchLinear,
       false, true},
      {"no-inlist-barrier", Kind::kCopy, DkImageFlags_Usage2DEngine, false, false},
  };

  // Blocks a timed-out copy may still be reading must outlive this call; the
  // session is over anyway, but freeing them could fault the 2D engine on top
  // of the stall and lose the log.
  static std::vector<std::unique_ptr<dk::UniqueMemBlock>> held_staging;
  static std::vector<std::unique_ptr<dk::UniqueMemBlock>> held_image_memory;

  REXGPU_WARN("[upload-probe] begin: {} variants, {}x{} RGBA8, pitch {}, {} ms bound each",
              uint32_t(sizeof(variants) / sizeof(variants[0])), kWidth, kHeight, kPitch,
              kFenceNs / 1000000);
  uint32_t copies_passed = 0;
  uint32_t copies_run = 0;
  bool fence_only_ok = false;
  bool clear_only_ok = false;

  for (const Variant& variant : variants) {
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{device_.device()}
        .setFlags(variant.image_flags)
        .setType(DkImageType_2D)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(kWidth, kHeight, 1)
        .setMipLevels(1)
        .initialize(layout);
    const uint64_t image_size = layout.getSize();
    const uint32_t image_alignment = layout.getAlignment();
    if (!image_size || !image_alignment) {
      REXGPU_WARN("[upload-probe] {}: layout rejected (size {}, align {})", variant.name,
                  image_size, image_alignment);
      continue;
    }
    const uint64_t image_stride =
        (image_size + image_alignment - 1) & ~uint64_t(image_alignment - 1);
    auto image_memory = std::make_unique<dk::UniqueMemBlock>(
        dk::MemBlockMaker{device_.device(), uint32_t(image_stride)}
            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
            .create());
    if (!*image_memory) {
      REXGPU_WARN("[upload-probe] {}: image memory allocation failed ({} bytes)", variant.name,
                  image_stride);
      continue;
    }
    dk::Image image{};
    image.initialize(layout, *image_memory, 0);

    const uint32_t staging_size =
        (kStagingBytes + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint32_t(DK_MEMBLOCK_ALIGNMENT - 1);
    const uint32_t staging_flags =
        variant.staging_uncached
            ? (DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            : (DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached);
    auto staging = std::make_unique<dk::UniqueMemBlock>(
        dk::MemBlockMaker{device_.device(), staging_size}.setFlags(staging_flags).create());
    if (!*staging) {
      REXGPU_WARN("[upload-probe] {}: staging allocation failed ({} bytes)", variant.name,
                  staging_size);
      continue;
    }

    upload_command_buffer_.clear();
    upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
    DkCopyBuf source{};
    source.addr = (*staging).getGpuAddr();
    source.rowLength = kPitch;
    source.imageHeight = kHeight;  // rows (run-061: not bytes)
    dk::ImageView destination_view{image};
    destination_view.setMipLevels(0, 1);
    const DkImageRect destination_rect{0, 0, 0, kWidth, kHeight, 1};
    bool flushed = true;
    switch (variant.kind) {
      case Kind::kFenceOnly:
        break;
      case Kind::kClearOnly:
        upload_command_buffer_.bindRenderTargets(&destination_view);
        upload_command_buffer_.setScissors(0, {{0, 0, kWidth, kHeight}});
        upload_command_buffer_.clearColor(0, DkColorMask_RGBA, 0.25f, 0.5f, 0.75f, 1.0f);
        break;
      case Kind::kCopy:
        std::memset((*staging).getCpuAddr(), 0x5A, staging_size);
        if (!variant.staging_uncached) {
          flushed = FlushCpuRange(*staging, 0, staging_size);
        }
        upload_command_buffer_.copyBufferToImage(source, destination_view, destination_rect);
        if (variant.barrier) {
          upload_command_buffer_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
        }
        break;
    }
    const DkCmdList list = upload_command_buffer_.finishList();

    dk::Fence fence{};
    const auto start = std::chrono::steady_clock::now();
    {
      std::lock_guard lock(device_.submit_mutex());
      device_.queue().submitCommands(list);
      device_.queue().signalFence(fence, true);
    }
    const DkResult result = fence.wait(kFenceNs);
    const int64_t elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    const bool ok = result == DkResult_Success;
    if (variant.kind == Kind::kCopy) {
      ++copies_run;
      if (ok) ++copies_passed;
    } else if (variant.kind == Kind::kFenceOnly) {
      fence_only_ok = ok;
    } else {
      clear_only_ok = ok;
    }
    REXGPU_WARN(
        "[upload-probe] {}: {} in {} ms (flags {:X}, staging {}, barrier {}, flush {}, "
        "queue error state {})",
        variant.name, ok ? "PASS" : "TIMEOUT", elapsed_ms, variant.image_flags,
        variant.staging_uncached ? "uncached" : "cached", variant.barrier ? "yes" : "no",
        variant.staging_uncached ? "n/a" : (flushed ? "yes" : "NO"),
        device_.queue().isInErrorState() ? 1 : 0);
    if (!ok) {
      // Never stop the run of variants: a lost fence does not block the queue,
      // and a real hang only makes the later waits look the same.
      held_staging.push_back(std::move(staging));
      held_image_memory.push_back(std::move(image_memory));
    }
  }

  REXGPU_WARN(
      "[upload-probe] summary: fence-only {}, clear-only {}, copies {}/{} completed - {}",
      fence_only_ok ? "PASS" : "TIMEOUT", clear_only_ok ? "PASS" : "TIMEOUT", copies_passed,
      copies_run,
      (!fence_only_ok || !clear_only_ok)
          ? "the FENCE is the problem: dkFenceWait never observed a signal for work that the "
            "queue demonstrably completed, so every copy verdict below is meaningless"
          : "the fences signal, so the copy verdicts are real");
  return fence_only_ok && clear_only_ok;
}

// Run-065: the copy diagnostic, as deterministic discriminators.
//
// Run 064's chain read back all zeros, which proves the chain produced no data but
// not which step failed: a readback that never executed also reads zero, and a
// blit that never executed leaves the destination at zero. So every step below
// runs with KNOWN CONTENT on both sides and is read back separately:
//
//   a  buffer -> buffer           (0x5A source, 0xAA destination)
//   b  buffer -> image -> buffer  (the texture-upload primitive)
//   c  image -> image  -> buffer  (the present blit; destination pre-cleared 0xC3)
//   d  buffer -> presenter-shaped image -> buffer (flagged: a swapchain-like image
//      may simply not be readable as a copy source, so a zero here proves nothing)
//
// Each list gets its own fence and a bounded 250 ms wait - fences are known to
// work (run 064: they miss only the first poll), and this runs ONCE, at the first
// real texture upload, not at boot. Every image logs its creation flags, size,
// align and the copy's rowLength/imageHeight so a units/flag mismatch is visible.
bool Deko3DTextureCache::RunCopyDiagnostic() {
  constexpr uint32_t kW = 64;
  constexpr uint32_t kH = 64;
  constexpr uint32_t kPitch = 4096;  // bytes per row, the real upload's padded pitch
  constexpr uint32_t kBytes = kPitch * kH;
  const uint32_t staging_size = (kBytes + DK_MEMBLOCK_ALIGNMENT - 1) &
                                ~uint32_t(DK_MEMBLOCK_ALIGNMENT - 1);
  auto make_block = [&](uint8_t fill) -> dk::UniqueMemBlock {
    dk::UniqueMemBlock b =
        dk::MemBlockMaker{device_.device(), staging_size}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
    if (b) {
      std::memset(b.getCpuAddr(), fill, staging_size);
    }
    return b;
  };

  dk::UniqueMemBlock src = make_block(0x5A);   // known source pattern
  dk::UniqueMemBlock dst = make_block(0xAA);   // buffer->buffer destination
  dk::UniqueMemBlock rb = make_block(0xAA);    // readback (pre-filled!)
  dk::UniqueMemBlock rb2 = make_block(0xAA);   // second readback
  if (!src || !dst || !rb || !rb2) {
    REXGPU_WARN("[copy-diag] allocation failed");
    return false;
  }
  // Row pattern: first 256 bytes of each row 0x5A, padding zero (as the real
  // upload writes it), so a shifted readback is visible too.
  auto* src_bytes = static_cast<uint8_t*>(src.getCpuAddr());
  std::memset(src_bytes, 0, staging_size);
  for (uint32_t y = 0; y < kH; ++y) {
    std::memset(src_bytes + size_t(y) * kPitch, 0x5A, kW * 4);
  }

  struct DiagImage {
    dk::UniqueMemBlock memory;
    dk::Image image{};
    uint32_t flags = 0;
    uint64_t size = 0;
    uint32_t align = 0;
    bool ok = false;
  };
  auto make_image = [&](DiagImage& out, uint32_t flags) {
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{device_.device()}
        .setFlags(flags)
        .setType(DkImageType_2D)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(kW, kH, 1)
        .setMipLevels(1)
        .initialize(layout);
    out.size = layout.getSize();
    out.align = layout.getAlignment();
    out.flags = flags;
    if (!out.size || !out.align) {
      REXGPU_WARN("[copy-diag] layout rejected for flags 0x{:X} (size {}, align {})", flags,
                  out.size, out.align);
      return;
    }
    const uint64_t stride = (out.size + out.align - 1) & ~uint64_t(out.align - 1);
    out.memory = dk::MemBlockMaker{device_.device(), uint32_t(stride)}
                     .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                     .create();
    if (!out.memory) {
      REXGPU_WARN("[copy-diag] image memory failed for flags 0x{:X}", flags);
      return;
    }
    out.image.initialize(layout, out.memory, 0);
    out.ok = true;
  };

  DiagImage img_plain;       // the texture-upload destination: Usage2DEngine only
  DiagImage img_present;     // presenter-shaped: Render | 2DEngine | HwCompression
  make_image(img_plain, DkImageFlags_Usage2DEngine);
  make_image(img_present, DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine |
                              DkImageFlags_HwCompression);

  // One list, own fence, bounded wait, and a verdict.
  auto run_list = [&](const char* name, const DkCmdList& list) {
    dk::Fence fence{};
    {
      std::lock_guard lock(device_.submit_mutex());
      device_.queue().submitCommands(list);
      device_.queue().signalFence(fence, true);
    }
    NoteGpuSubmit();
    REXGPU_WARN("[submit] {} site=texture_cache standalone queue=0x{:X} device=0x{:X} "
                "fence={} cmdbuf={}",
                name, uint64_t(device_.queue()), uint64_t(device_.device()),
                static_cast<const void*>(&fence), static_cast<const void*>(&upload_command_buffer_));
    const bool ok = fence.wait(250000000) == DkResult_Success;
    REXGPU_WARN("[copy-diag] {} fence {}", name, ok ? "signalled" : "TIMEOUT (250 ms)");
    return ok;
  };
  auto hex8 = [](const uint8_t* p) {
    static char out[40];
    std::snprintf(out, sizeof(out), "%02X%02X%02X%02X %02X%02X%02X%02X", p[0], p[1], p[2], p[3],
                  p[4], p[5], p[6], p[7]);
    return out;
  };

  uint32_t pass_mask = 0;
  const DkImageRect rect{0, 0, 0, kW, kH, 1};

  // ---- a: buffer -> buffer ------------------------------------------------
  {
    upload_command_buffer_.clear();
    upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
    upload_command_buffer_.copyBuffer(src.getGpuAddr(), dst.getGpuAddr(), kBytes);
    const DkCmdList list = upload_command_buffer_.finishList();
    const bool fence_ok = run_list("a buffer->buffer", list);
    const uint8_t* d = static_cast<const uint8_t*>(dst.getCpuAddr());
    const bool ok = fence_ok && d[0] == 0x5A && d[1] == 0x5A;
    if (ok) pass_mask |= 1;
    REXGPU_WARN("[copy-diag] a buffer->buffer: pre=AAAAAAAA post=[{}] {} {} bytes", hex8(d), ok ? "PASS" : "FAIL", kBytes);
  }

  // ---- b: buffer -> image (plain) -> buffer -------------------------------
  {
    const uint8_t* pre = static_cast<const uint8_t*>(rb.getCpuAddr());
    REXGPU_WARN("[copy-diag] b image A: flags 0x{:X} size {} align {}, copy rowLength {} bytes "
                "imageHeight {} rows, readback pre=[{}]",
                img_plain.flags, img_plain.size, img_plain.align, kPitch, kH, hex8(pre));
    bool fence_ok = false;
    if (img_plain.ok) {
      upload_command_buffer_.clear();
      upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
      dk::ImageView va{img_plain.image};
      va.setMipLevels(0, 1);
      DkCopyBuf to_image{};
      to_image.addr = src.getGpuAddr();
      to_image.rowLength = kPitch;
      to_image.imageHeight = kH;
      upload_command_buffer_.copyBufferToImage(to_image, va, rect);
      upload_command_buffer_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
      DkCopyBuf from_image{};
      from_image.addr = rb.getGpuAddr();
      from_image.rowLength = kPitch;
      from_image.imageHeight = kH;
      upload_command_buffer_.copyImageToBuffer(va, rect, from_image, 0);
      const DkCmdList list = upload_command_buffer_.finishList();
      fence_ok = run_list("b buffer->image->buffer", list);
    }
    const uint8_t* d = static_cast<const uint8_t*>(rb.getCpuAddr());
    const bool copied = fence_ok && d[0] == 0x5A;
    const bool untouched = fence_ok && d[0] == 0xAA;
    if (copied) pass_mask |= 2;
    REXGPU_WARN("[copy-diag] b buffer->image->buffer: post=[{}] {}", hex8(d),
                copied ? "PASS (image holds the pattern)"
                       : untouched ? "FAIL: readback still the pre-fill 0xAA - the "
                                     "image->buffer copy did not execute or targeted another block"
                                   : "FAIL: readback changed but is not the pattern (addressing/format)");
  }

  // ---- c: image -> image (present blit), destination pre-cleared 0xC3 ------
  {
    bool fence_ok = false;
    if (img_plain.ok && img_present.ok) {
      upload_command_buffer_.clear();
      upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
      dk::ImageView vb{img_present.image};
      vb.setMipLevels(0, 1);
      upload_command_buffer_.bindRenderTargets(&vb);
      upload_command_buffer_.setScissors(0, {{0, 0, kW, kH}});
      upload_command_buffer_.clearColor(0, DkColorMask_RGBA, 0.7647f, 0.7647f, 0.7647f, 0.7647f);
      const DkCmdList clear_list = upload_command_buffer_.finishList();
      const bool clear_ok = run_list("c clear B to 0xC3", clear_list);
      fence_ok = clear_ok;
      if (clear_ok) {
        upload_command_buffer_.clear();
        upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
        dk::ImageView va{img_plain.image};
        va.setMipLevels(0, 1);
        upload_command_buffer_.copyImage(va, rect, vb, rect, 0);
        upload_command_buffer_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
        DkCopyBuf from_image{};
        from_image.addr = rb2.getGpuAddr();
        from_image.rowLength = kPitch;
        from_image.imageHeight = kH;
        upload_command_buffer_.copyImageToBuffer(vb, rect, from_image, 0);
        const DkCmdList list = upload_command_buffer_.finishList();
        fence_ok = run_list("c image->image->buffer", list);
      }
    }
    const uint8_t* d = static_cast<const uint8_t*>(rb2.getCpuAddr());
    const bool blitted = fence_ok && d[0] == 0x5A;
    const bool still_clear = fence_ok && d[0] == 0xC3;
    if (blitted) pass_mask |= 4;
    REXGPU_WARN("[copy-diag] c image->image (present blit): B flags 0x{:X}, readback post=[{}] {}",
                img_present.flags, hex8(d),
                blitted ? "PASS (blit wrote the source pattern)"
                        : still_clear ? "FAIL: B still holds its 0xC3 clear - the blit wrote nothing"
                                      : "FAIL: neither clear nor pattern (readback path)");
  }

  // ---- d: buffer -> presenter-shaped image -> buffer (flagged) ------------
  {
    bool fence_ok = false;
    if (img_present.ok) {
      upload_command_buffer_.clear();
      upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
      dk::ImageView vb{img_present.image};
      vb.setMipLevels(0, 1);
      DkCopyBuf to_image{};
      to_image.addr = src.getGpuAddr();
      to_image.rowLength = kPitch;
      to_image.imageHeight = kH;
      upload_command_buffer_.copyBufferToImage(to_image, vb, rect);
      upload_command_buffer_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
      DkCopyBuf from_image{};
      from_image.addr = dst.getGpuAddr();
      from_image.rowLength = kPitch;
      from_image.imageHeight = kH;
      upload_command_buffer_.copyImageToBuffer(vb, rect, from_image, 0);
      const DkCmdList list = upload_command_buffer_.finishList();
      fence_ok = run_list("d buffer->presenter-image->buffer", list);
    }
    const uint8_t* d = static_cast<const uint8_t*>(dst.getCpuAddr());
    const bool ok = fence_ok && d[0] == 0x5A;
    if (ok) pass_mask |= 8;
    REXGPU_WARN("[copy-diag] d buffer->presenter-image->buffer (FLAGGED: a swapchain-like image may "
                "not be readable): post=[{}] {}", hex8(d), ok ? "PASS" : "inconclusive/fail");
  }

  REXGPU_WARN("[copy-diag] summary: a(buffer->buffer)={} b(buffer->image)={} c(image->image)={} "
              "d(presenter-image)={}",
              (pass_mask & 1) ? "PASS" : "FAIL", (pass_mask & 2) ? "PASS" : "FAIL",
              (pass_mask & 4) ? "PASS" : "FAIL", (pass_mask & 8) ? "PASS" : "inconclusive");
  return pass_mask == 0xF;
}

// Run-067: copy-only bisect with a real texture's parameters.
//
// Runs at setup, before the first frame, so nothing else can kill the process
// first. Two destinations are tried with the SAME source, one after the other:
//
//   tiled        exactly the flags/format/tiling of a real upload (#1 in run 066:
//                960x720, xenos format 23 -> RGBA8, DkImageFlags_Usage2DEngine)
//   pitch-linear the same image as DkImageFlags_PitchLinear, whose actual row pitch
//                deko3d reports (size / height), printed next to the rowLength we
//                pass in DkCopyBuf - if those disagree, that is the bug.
//
// Each step: record ONLY the copy, submit it with its own fence, wait 1 s, and (if
// the fence signalled) copy 64x64 back and print the pixels. Every step logs what
// it submitted BEFORE submitting, so a death names its own point.
bool Deko3DTextureCache::RunCopyBisect() {
  constexpr uint32_t kW = 960;   // upload #1 width
  constexpr uint32_t kH = 720;   // upload #1 height
  constexpr uint32_t kSrcPitch = 4096;  // the padded staging pitch the port uses
  constexpr uint32_t kSrcBytes = kSrcPitch * kH;

  const uint32_t staging_size = (kSrcBytes + DK_MEMBLOCK_ALIGNMENT - 1) &
                                ~uint32_t(DK_MEMBLOCK_ALIGNMENT - 1);
  dk::UniqueMemBlock staging =
      dk::MemBlockMaker{device_.device(), staging_size}
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
  dk::UniqueMemBlock readback =
      dk::MemBlockMaker{device_.device(), 4096 * 64}
          .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
          .create();
  if (!staging || !readback) {
    REXGPU_WARN("[copy-bisect] allocation failed");
    return false;
  }
  auto* src = static_cast<uint8_t*>(staging.getCpuAddr());
  std::memset(src, 0, staging_size);
  for (uint32_t y = 0; y < kH; ++y) {
    std::memset(src + size_t(y) * kSrcPitch, 0x5A, kW * 4);  // 3840 bytes of data
  }
  std::memset(readback.getCpuAddr(), 0xAA, 4096 * 64);

  struct Attempt {
    const char* name;
    uint32_t flags;
  };
  const Attempt attempts[] = {
      {"tiled (real upload #1 flags)", DkImageFlags_Usage2DEngine},
      {"pitch-linear", DkImageFlags_Usage2DEngine | DkImageFlags_PitchLinear},
  };

  bool any_pass = false;
  for (const Attempt& attempt : attempts) {
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{device_.device()}
        .setFlags(attempt.flags)
        .setType(DkImageType_2D)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(kW, kH, 1)
        .setMipLevels(1)
        .initialize(layout);
    const uint64_t size = layout.getSize();
    const uint32_t align = layout.getAlignment();
    if (!size || !align) {
      REXGPU_WARN("[copy-bisect] {}: layout rejected (size {}, align {}) - this destination "
                  "cannot even be created", attempt.name, size, align);
      continue;
    }
    // deko3d does not expose a block-linear pitch; for the pitch-linear layout the
    // size is (aligned) pitch * height, so the pitch is recoverable and can be
    // compared with the rowLength we pass.
    const uint64_t derived_pitch = size / kH;
    REXGPU_WARN("[copy-bisect] {}: flags 0x{:X} size {} align {} (derived pitch {} bytes) | "
                "we pass rowLength {} bytes, imageHeight {} rows, rect {}x{}",
                attempt.name, attempt.flags, size, align, derived_pitch, kSrcPitch, kH, kW, kH);
    const uint64_t stride = (size + align - 1) & ~uint64_t(align - 1);
    dk::UniqueMemBlock image_memory =
        dk::MemBlockMaker{device_.device(), uint32_t(stride)}
            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
            .create();
    if (!image_memory) {
      REXGPU_WARN("[copy-bisect] {}: image memory failed ({} bytes)", attempt.name, stride);
      continue;
    }
    dk::Image image{};
    image.initialize(layout, image_memory, 0);
    dk::ImageView view{image};
    view.setMipLevels(0, 1);
    const DkImageRect full_rect{0, 0, 0, kW, kH, 1};

    // --- copy only, own fence, bounded wait ------------------------------
    REXGPU_WARN("[copy-bisect] {}: submitting copyBufferToImage only ({}x{}, rowLength {} B, "
                "imageHeight {} rows)", attempt.name, kW, kH, kSrcPitch, kH);
    {
      upload_command_buffer_.clear();
      upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
      DkCopyBuf to_image{};
      to_image.addr = staging.getGpuAddr();
      to_image.rowLength = kSrcPitch;
      to_image.imageHeight = kH;
      upload_command_buffer_.copyBufferToImage(to_image, view, full_rect);
      const DkCmdList list = upload_command_buffer_.finishList();
      dk::Fence fence{};
      {
        std::lock_guard lock(device_.submit_mutex());
        device_.queue().submitCommands(list);
        device_.queue().signalFence(fence, true);
      }
      NoteGpuSubmit();
      const bool ok = fence.wait(1000000000) == DkResult_Success;
      REXGPU_WARN("[copy-bisect] {}: copy fence {} after up to 1000 ms", attempt.name,
                  ok ? "SIGNALLED" : "TIMEOUT");
      if (!ok) {
        continue;  // nothing to read back
      }
    }

    // --- read 64x64 of the destination back ------------------------------
    {
      upload_command_buffer_.clear();
      upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
      DkCopyBuf back{};
      back.addr = readback.getGpuAddr();
      back.rowLength = 4096;
      back.imageHeight = 64;
      const DkImageRect read_rect{0, 0, 0, 64, 64, 1};
      upload_command_buffer_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
      upload_command_buffer_.copyImageToBuffer(view, read_rect, back, 0);
      const DkCmdList list = upload_command_buffer_.finishList();
      dk::Fence fence{};
      {
        std::lock_guard lock(device_.submit_mutex());
        device_.queue().submitCommands(list);
        device_.queue().signalFence(fence, true);
      }
      NoteGpuSubmit();
      const bool ok = fence.wait(1000000000) == DkResult_Success;
      auto* rb = static_cast<uint8_t*>(readback.getCpuAddr());
      const bool pattern = rb[0] == 0x5A && rb[1] == 0x5A;
      if (ok && pattern) {
        any_pass = true;
      }
      REXGPU_WARN("[copy-bisect] {}: readback fence {} | first pixels [{:02X}{:02X}{:02X}{:02X} "
                  "{:02X}{:02X}{:02X}{:02X}] - {}",
                  attempt.name, ok ? "SIGNALLED" : "TIMEOUT", rb[0], rb[1], rb[2], rb[3], rb[4],
                  rb[5], rb[6], rb[7],
                  !ok ? "readback did not execute"
                      : (pattern ? "COPY OK (pattern landed in the image)"
                                 : "copy ran but the image reads back something else"));
    }
  }
  REXGPU_WARN("[copy-bisect] done: {} - {}", any_pass ? "at least one destination works"
                                                      : "NO destination worked",
              any_pass ? "the copy is not the killer; the next suspect is the draw that "
                         "samples the uploaded image"
                       : "the copy alone is the killer (see which attempt died and where)");
  return any_pass;
}

// Run-068: does that one copy execute? A synchronous answer at setup.
//
// Host-side finding (disassembly of the installed libdeko3d.a + libnx
// gpu_channel.o, byte-identical to the WSL toolchain the NRO links): neither
// submitCommands nor signalFence kicks the GPU channel - they append GPFIFO
// entries to a 2048-entry batch. The doorbell (nvGpuChannelKickoff) fires only
// from Queue::flush, which the frame loop reaches via presentImage, and from
// waitIdle (signalFence + flush + wait). So every standalone copy test so far
// (058 boot probes, 065 a-d, 067 bisect) timed out WITHOUT the GPU ever being
// told to fetch the entries: 065 even blocked the presenting thread inside its
// fence waits (present #179 -> #180 gap = the four 250 ms waits). Fences are a
// monotonic per-channel semaphore, so a later present fence signalling
// retroactively satisfies all earlier fences on the queue. This test uses
// waitIdle as the witness precisely because it flushes/kicks. Unverified on
// hardware until run 068 returns.
//
// Run 067 left a contradiction: the copy's fence timed out for 1000 ms and then the
// queue presented frames happily - which is far more consistent with the copy
// submission never being executed than with a wedged 2D engine. This test cannot
// lie in either direction, because it does not trust the fence:
//
//   1. record the 960x720 buffer->image copy into a fresh list, log the list handle
//      and the command buffer it came from, submit it;
//   2. log the fence state BEFORE (wait 0) and AFTER `dkQueueWaitIdle`, run in a
//      thread with a 2 s observation window so the log always returns;
//   3. read 64x64 of the destination back and print the pixels.
//
// Descriptor A runs by default; B and C are opt-in (deko3d_copy_sync_extra),
// because the units of DkCopyBuf are the one thing the header does not document
// (deko3d's own samples pass 0/0 for a tightly packed source, which is Vulkan's
// convention - rowLength in bytes, imageHeight in ROWS):
//
//   A  rowLength 4096, imageHeight 720          (rows; what the port passes now)
//   B  rowLength 4096, imageHeight 2949120      (total bytes; the old reading)
//   C  rowLength 0,    imageHeight 0            (tightly packed, 3840-byte rows)
//
// Verdict per attempt: EXECUTED (pattern read back), DROPPED (WaitIdle returned but
// the destination is untouched), WEDGED (WaitIdle never returned).
bool Deko3DTextureCache::RunCopySyncTest() {
  constexpr uint32_t kW = 960;
  constexpr uint32_t kH = 720;
  constexpr uint32_t kTightPitch = kW * 4;      // 3840
  constexpr uint32_t kPaddedPitch = 4096;

  dk::ImageLayout layout;
  dk::ImageLayoutMaker{device_.device()}
      .setFlags(DkImageFlags_Usage2DEngine)
      .setType(DkImageType_2D)
      .setFormat(DkImageFormat_RGBA8_Unorm)
      .setDimensions(kW, kH, 1)
      .setMipLevels(1)
      .initialize(layout);
  const uint64_t image_size = layout.getSize();
  const uint32_t image_align = layout.getAlignment();
  if (!image_size || !image_align) {
    REXGPU_WARN("[copy-sync] image layout rejected");
    return false;
  }
  const uint64_t stride = (image_size + image_align - 1) & ~uint64_t(image_align - 1);
  dk::UniqueMemBlock image_memory =
      dk::MemBlockMaker{device_.device(), uint32_t(stride)}
          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
          .create();
  if (!image_memory) {
    REXGPU_WARN("[copy-sync] image allocation failed ({} bytes)", stride);
    return false;
  }
  dk::Image image{};
  image.initialize(layout, image_memory, 0);
  dk::ImageView view{image};
  view.setMipLevels(0, 1);

  // CpuUncached, not CpuCached: the CPU pre-fills 0xAA and later reads the
  // result. With a CPU-cached block those reads hit the CPU's own dirty 0xAA
  // lines even when the GPU wrote the pattern to DRAM - a fake DROPPED verdict.
  // This deko3d exposes no CPU-cache invalidate (only dkMemBlockFlushCpuCache),
  // so uncached CPU access is the only coherent readback here. Uncached writes
  // go straight to DRAM, which is also what the 2D engine reads.
  dk::UniqueMemBlock readback =
      dk::MemBlockMaker{device_.device(), 4096 * 64}
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
  if (!readback) {
    REXGPU_WARN("[copy-sync] readback allocation failed");
    return false;
  }

  // Bounded observation of dkQueueWaitIdle: it has no timeout of its own.
  auto bounded_idle = [&](const char* where) -> bool {
    std::atomic<bool> returned{false};
    std::thread worker([this, &returned]() {
      device_.queue().waitIdle();
      returned.store(true, std::memory_order_release);
    });
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 400 && !returned.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    const bool ok = returned.load(std::memory_order_acquire);
    if (ok) {
      worker.join();
    } else {
      worker.detach();  // the queue thread stays parked; the log must continue
    }
    REXGPU_WARN("[copy-sync] {}: dkQueueWaitIdle {} after {} ms", where,
                ok ? "RETURNED" : "DID NOT RETURN (2 s window)", ms);
    return ok;
  };

  struct Attempt {
    const char* name;
    uint32_t pitch;
    uint32_t row_length;
    uint32_t image_height;
  };
  const Attempt attempts[] = {
      {"A rows (rowLength 4096, imageHeight 720)", kPaddedPitch, kPaddedPitch, kH},
      {"B bytes (rowLength 4096, imageHeight 2949120)", kPaddedPitch, kPaddedPitch, kPaddedPitch * kH},
      {"C tight (rowLength 0, imageHeight 0, 3840 pitch)", kTightPitch, 0, 0},
  };

  // One measured variable per run: A alone decides EXECUTED/DROPPED/WEDGED for
  // the descriptor the port actually uses. B is the run-060 OS-kill DMA (12 GB
  // of out-of-bounds source reads) if it ever executes, and C answers nothing
  // once A has decided - both stay behind an opt-in cvar for a later A/B.
  const bool extra = REXCVAR_GET(deko3d_copy_sync_extra);
  for (size_t i = 0; i < 3; ++i) {
    if (i > 0 && !extra) {
      break;
    }
    const Attempt& attempt = attempts[i];
    const uint32_t bytes = attempt.pitch * kH;
    const uint32_t size = (bytes + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint32_t(DK_MEMBLOCK_ALIGNMENT - 1);
    dk::UniqueMemBlock staging =
        dk::MemBlockMaker{device_.device(), size}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
    if (!staging) {
      REXGPU_WARN("[copy-sync] {}: staging allocation failed", attempt.name);
      continue;
    }
    auto* src = static_cast<uint8_t*>(staging.getCpuAddr());
    std::memset(src, 0, size);
    for (uint32_t y = 0; y < kH; ++y) {
      std::memset(src + size_t(y) * attempt.pitch, 0x5A, kW * 4);
    }
    std::memset(readback.getCpuAddr(), 0xAA, 4096 * 64);

    upload_command_buffer_.clear();
    upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
    DkCopyBuf to_image{};
    to_image.addr = staging.getGpuAddr();
    to_image.rowLength = attempt.row_length;
    to_image.imageHeight = attempt.image_height;
    const DkImageRect rect{0, 0, 0, kW, kH, 1};
    upload_command_buffer_.copyBufferToImage(to_image, view, rect);
    const DkCmdList list = upload_command_buffer_.finishList();
    REXGPU_WARN("[copy-sync] {}: list=0x{:X} from cmdbuf={} (1 command: copyBufferToImage {}x{}, "
                "src 0x{:X} pitch {})",
                attempt.name, uint64_t(list), static_cast<const void*>(&upload_command_buffer_), kW,
                kH, uint64_t(staging.getGpuAddr()), attempt.pitch);

    dk::Fence fence{};
    {
      std::lock_guard lock(device_.submit_mutex());
      device_.queue().submitCommands(list);
      device_.queue().signalFence(fence, true);
    }
    NoteGpuSubmit();
    const bool fence_before = fence.wait(0) == DkResult_Success;
    REXGPU_WARN("[copy-sync] {}: fence state before WaitIdle = {}", attempt.name,
                fence_before ? "signalled" : "not signalled");
    const bool idle_ok = bounded_idle(attempt.name);
    const bool fence_after = fence.wait(0) == DkResult_Success;
    REXGPU_WARN("[copy-sync] {}: fence state after WaitIdle = {} (queue error state: {})",
                attempt.name, fence_after ? "signalled" : "not signalled",
                device_.queue().isInErrorState() ? "YES - channel faulted" : "no");

    if (!idle_ok) {
      REXGPU_WARN("[copy-sync] {}: verdict WEDGED - the queue never went idle; this copy is the "
                  "wedge and the units/flags question is now worth pursuing", attempt.name);
      break;  // the queue is dead; later attempts would only pile up parked threads
    }

    // Read 64x64 back; the queue is idle, so this list executes immediately.
    upload_command_buffer_.clear();
    upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
    upload_command_buffer_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    DkCopyBuf back{};
    back.addr = readback.getGpuAddr();
    back.rowLength = 4096;
    back.imageHeight = 64;
    const DkImageRect read_rect{0, 0, 0, 64, 64, 1};
    upload_command_buffer_.copyImageToBuffer(view, read_rect, back, 0);
    const DkCmdList rb_list = upload_command_buffer_.finishList();
    {
      std::lock_guard lock(device_.submit_mutex());
      device_.queue().submitCommands(rb_list);
    }
    const bool rb_idle = bounded_idle("readback");
    auto* rb = static_cast<uint8_t*>(readback.getCpuAddr());
    const bool pattern = rb[0] == 0x5A && rb[1] == 0x5A;
    REXGPU_WARN("[copy-sync] {}: readback {} first pixels [{:02X}{:02X}{:02X}{:02X} {:02X}{:02X}"
                "{:02X}{:02X}]",
                attempt.name, rb_idle ? "ran" : "did not run", rb[0], rb[1], rb[2], rb[3], rb[4],
                rb[5], rb[6], rb[7]);
    REXGPU_WARN("[copy-sync] {}: verdict {}", attempt.name,
                pattern ? "EXECUTED - the copy landed the pattern (this is the working descriptor)"
                        : "DROPPED - WaitIdle returned but the destination is untouched, so the "
                          "command is not reaching the GPU");
  }
  return true;
}

std::unique_ptr<TextureCache::Texture> Deko3DTextureCache::CreateTexture(TextureKey key) {
  DkImageFormat format;
  if (!GetDekoImageFormat(key.format, format)) {
    static bool logged_unsupported = false;
    if (!logged_unsupported) {
      logged_unsupported = true;
      REXGPU_WARN("deko3d: unsupported texture format {} (0x{:08X}), skipping",
                  uint32_t(key.format), uint32_t(key.format));
    }
    return nullptr;
  }

  const uint32_t width = key.width_minus_1 + 1;
  const uint32_t height = key.height_minus_1 + 1;
  const uint32_t depth = key.depth_or_array_size_minus_1 + 1;
  DkImageType type = DkImageType_2D;
  switch (key.dimension) {
    case xenos::DataDimension::k1D:
      type = DkImageType_1D;
      break;
    case xenos::DataDimension::k2DOrStacked:
      type = depth > 1 ? DkImageType_2DArray : DkImageType_2D;
      break;
    case xenos::DataDimension::k3D:
      type = DkImageType_3D;
      break;
    case xenos::DataDimension::kCube:
      type = DkImageType_Cubemap;
      break;
    default:
      return nullptr;
  }

  std::unique_ptr<TextureImpl> texture = std::make_unique<TextureImpl>(*this, key);
  if (!texture->Initialize(format, type, width, height, depth)) {
    return nullptr;
  }
  return texture;
}

bool Deko3DTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture,
                                                               bool load_base,
                                                               bool load_mips) {
  (void)load_mips;
  if (!load_base) {
    return true;
  }
  if (!REXCVAR_GET(deko3d_texture_upload)) {
    // Pretend the load succeeded: the draw path samples the placeholder anyway,
    // and reporting failure would make the texture cache retry every request.
    return true;
  }
  if (upload_stopped_) {
    return false;
  }
  TextureImpl& deko_texture = static_cast<TextureImpl&>(texture);
  const TextureKey& key = texture.key();
  const texture_util::TextureGuestLayout& guest_layout = texture.guest_layout();
  const texture_util::TextureGuestLayout::Level& base = guest_layout.base;
  if (!base.level_data_extent_bytes) {
    return true;
  }

  // The base-level-only upload is enough to start; mips and arrays are a
  // follow-up once the DKSH pipeline can sample them.
  if (key.depth_or_array_size_minus_1 != 0 ||
      key.dimension == xenos::DataDimension::k3D ||
      key.dimension == xenos::DataDimension::kCube) {
    static bool logged_unsupported = false;
    if (!logged_unsupported) {
      logged_unsupported = true;
      REXGPU_WARN("deko3d: array/3D/cube texture upload not implemented yet; "
                  "using the base level only");
    }
    return true;
  }

  const FormatInfo* format_info = FormatInfo::Get(key.format);
  if (!format_info) {
    return false;
  }
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  const uint32_t width = deko_texture.width();
  const uint32_t height = deko_texture.height();
  const uint32_t blocks_wide = (width + format_info->block_width - 1) /
                               format_info->block_width;
  const uint32_t blocks_high = (height + format_info->block_height - 1) /
                               format_info->block_height;
  const uint32_t row_bytes = blocks_wide * bytes_per_block;
  const uint32_t total_bytes = row_bytes * blocks_high;
  if (!total_bytes) {
    return true;
  }

  // Source row pitch for the copy. A 3840-byte row (960 px of RGBA8) wedged the
  // 2D engine permanently, while the presenter's 5120-byte swapchain rows have
  // been copied for thousands of frames without trouble, so the pitch is the
  // one known-bad difference. Pad to a 4096-byte multiple - a multiple of every
  // plausible tile and pitch granularity - and zero the padding.
  constexpr uint32_t kSourcePitchAlignment = 4096;
  const uint32_t source_pitch =
      (row_bytes + kSourcePitchAlignment - 1) & ~(kSourcePitchAlignment - 1);
  const uint32_t staging_bytes = source_pitch * blocks_high;

  const xenos::TextureFormat base_format = GetBaseFormat(key.format);
  const bool block_compressed = base_format == xenos::TextureFormat::k_DXT1 ||
                                base_format == xenos::TextureFormat::k_DXT2_3 ||
                                base_format == xenos::TextureFormat::k_DXT4_5 ||
                                base_format == xenos::TextureFormat::k_DXN;
  static uint32_t logged_uploads = 0;
  if (logged_uploads < 8) {
    ++logged_uploads;
    REXGPU_WARN(
        "deko3d: texture upload {} of {}x{} format {} -> deko {}, block {}, row {} "
        "bytes (padded to {}), total {} bytes, tiled {}, {} layers",
        logged_uploads, width, height, uint32_t(key.format), uint32_t(deko_texture.format()),
        int(block_compressed), row_bytes, source_pitch, staging_bytes, int(key.tiled),
        key.depth_or_array_size_minus_1 + 1);
  }
  // Not refused: the draw path now waits the previous list before an upload
  // submits its own, so a stall names its own author - a timeout naming "a
  // texture upload" is the copy, one naming the render list is the clear or draw
  // that wedged the queue behind it. Refusing these shapes would have hidden that
  // answer and left most of this title's DXT textures un-uploaded.

  // The size must be a whole number of pages: deko3d allocates the block from
  // the GPU memory pool at page granularity, and a copy that reads past the end
  // of the mapped region is an invalid DMA. (The run-034 "permanent wedge" was
  // the imageHeight-units bug - 2.9M rows instead of 720 - fixed in run 061;
  // run 068 proved the corrected descriptor executes and lands the pattern.)
  // Round the block to whole pages as well as covering the padded pitch.
  const uint32_t staging_size =
      (staging_bytes + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint32_t(DK_MEMBLOCK_ALIGNMENT - 1);
  // Owned through a unique_ptr: with deko3d_upload_no_wait the block outlives
  // this call (it is kept until its copy fence is observed), and a UniqueMemBlock
  // is a handle that cannot be moved into a container.
  const bool staging_uncached = REXCVAR_GET(deko3d_upload_staging_uncached);
  auto staging = std::make_unique<dk::UniqueMemBlock>(
      dk::MemBlockMaker{device_.device(), staging_size}
          .setFlags(staging_uncached ? (DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                     : (DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached))
          .create());
  if (!*staging) {
    REXGPU_ERROR("deko3d: texture staging allocation failed ({} bytes)", staging_size);
    return false;
  }

  const uint8_t* source = static_cast<Deko3DSharedMemory&>(shared_memory()).cpu_address() +
                          (key.base_page << 12);
  uint8_t* destination = static_cast<uint8_t*>((*staging).getCpuAddr());
  // Zero first so the pitch padding between rows is defined; the copy reads the
  // whole padded rows.
  std::memset(destination, 0, staging_size);
  const uint32_t endian = uint32_t(key.endianness);
  auto copy_block = [endian](void* output, const void* input, size_t length) {
    texture_conversion::CopySwapBlock(xenos::Endian(endian), output, input, length);
  };

  if (key.tiled) {
    texture_conversion::UntileInfo untile_info{};
    untile_info.offset_x = 0;
    untile_info.offset_y = 0;
    untile_info.width = blocks_wide;
    untile_info.height = blocks_high;
    untile_info.input_pitch = base.row_pitch_bytes / bytes_per_block;
    // Untile writes whole rows at this pitch, so the padded stride is applied
    // here rather than by copying row by row afterwards.
    untile_info.output_pitch = source_pitch / bytes_per_block;
    untile_info.input_format_info = format_info;
    untile_info.output_format_info = format_info;
    untile_info.copy_callback = copy_block;
    texture_conversion::Untile(destination, source, &untile_info);
  } else {
    for (uint32_t y = 0; y < blocks_high; ++y) {
      copy_block(destination + size_t(y) * source_pitch,
                 source + size_t(y) * base.row_pitch_bytes, row_bytes);
    }
  }

  if (!staging_uncached && !FlushCpuRange(*staging, 0, staging_size)) {
    REXGPU_ERROR("deko3d: texture staging cache flush failed");
    return false;
  }
  // Run-061 sync audit: the flush pushes the CPU writes out, and this release
  // fence orders them before the submit that publishes the block to the GPU (the
  // same class of ordering the UnleashedRecomp-NX fork restored ARM barriers
  // for). The matching acquire is below, before a block is handed out again.
  std::atomic_thread_fence(std::memory_order_release);

  // Run-064: name every upload before its copy, with the memory left, so a
  // limit/allocation death at ~10 s has a named author instead of looking like a
  // race.
  static uint32_t upload_index = 0;
  ++upload_index;
  NoteTextureUpload();
  if (upload_index <= 24) {
    REXGPU_WARN("[upload] #{} {}x{} format {} -> deko {} ({} bytes staging, tiled {}, {} layers)",
                upload_index, width, height, uint32_t(key.format),
                uint32_t(deko_texture.format()), staging_size, int(key.tiled),
                key.depth_or_array_size_minus_1 + 1);
    device_.LogMemory("texture upload");
  }

  // Run-066: record into the FRAME's command list when the command processor can
  // give us one: the frame's presentImage kicks the channel, so the copy executes
  // with the frame. Run-068 correction: run 065's "standalone lists never reach
  // the GPU" was a missing-kickoff artefact (submitCommands/signalFence only
  // append GPFIFO entries; only flush/presentImage/waitIdle kick) - the copy
  // itself EXECUTES and lands the pattern (descriptor A: rowLength in bytes,
  // imageHeight in rows). The immediate-submit path below survives as the
  // fallback and now flushes explicitly so its fence can signal.
  dk::CmdBuf* frame_cmdbuf = frame_recorder_ ? frame_recorder_() : nullptr;
  if (frame_cmdbuf) {
    DkCopyBuf to_image{};
    to_image.addr = (*staging).getGpuAddr();
    to_image.rowLength = source_pitch;
    to_image.imageHeight = blocks_high;
    dk::ImageView destination_view{deko_texture.image()};
    destination_view.setMipLevels(0, 1);
    const DkImageRect destination_rect{0, 0, 0, width, height, 1};
    frame_cmdbuf->copyBufferToImage(to_image, destination_view, destination_rect);
    frame_cmdbuf->barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    static uint32_t frame_recorded = 0;
    ++frame_recorded;
    if (frame_recorded <= 8 || (frame_recorded % 128) == 0) {
      REXGPU_WARN("[upload] #{} {}x{} recorded into the FRAME list (copy executes with the frame, "
                  "rowLength {} bytes, imageHeight {} rows)",
                  frame_recorded, width, height, source_pitch, blocks_high);
    }
    // The block must outlive the frame that reads it; the frame fence tells us, so
    // retirement keys off the frame's own signal (or age as the last resort).
    static std::vector<std::unique_ptr<dk::UniqueMemBlock>> frame_staging;
    static std::vector<uint64_t> frame_epochs;
    const uint64_t now_present = PresentEpoch();
    for (size_t i = frame_staging.size(); i-- > 0;) {
      if (now_present >= frame_epochs[i] && (now_present - frame_epochs[i]) >= 2) {
        frame_staging.erase(frame_staging.begin() + ptrdiff_t(i));
        frame_epochs.erase(frame_epochs.begin() + ptrdiff_t(i));
      }
    }
    frame_staging.push_back(std::move(staging));
    frame_epochs.push_back(now_present);

    // Run-066 acceptance: prove the FRAME-path upload lands pixels. On the first
    // recorded upload, also record a 64x64 copy back from the image into a block
    // pre-filled with 0xAA; a later upload logs those bytes once the frame that
    // carried them has been presented twice.
    if (!g_frame_probe.armed && frame_recorded == 1) {
      g_frame_probe.armed = true;
      g_frame_probe.block = dk::MemBlockMaker{device_.device(), 4096 * 64}
                        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                        .create();
      if (g_frame_probe.block) {
        std::memset(g_frame_probe.block.getCpuAddr(), 0xAA, 4096 * 64);
        const uint32_t pw = width < 64 ? width : 64;
        const uint32_t ph = height < 64 ? height : 64;
        DkCopyBuf back{};
        back.addr = g_frame_probe.block.getGpuAddr();
        back.rowLength = 4096;
        back.imageHeight = ph;
        dk::ImageView probe_view{deko_texture.image()};
        probe_view.setMipLevels(0, 1);
        const DkImageRect probe_rect{0, 0, 0, pw, ph, 1};
        frame_cmdbuf->copyImageToBuffer(probe_view, probe_rect, back, 0);
        g_frame_probe.present_epoch = PresentEpoch();
        REXGPU_WARN("[upload] probe armed: {}x{} of the first frame-path upload will be read back",
                    pw, ph);
      }
    }
    // Run-069: the readback is printed by PollFrameUploadProbe(), called from the
    // presenter on every present - not by a later upload (run 068: death came
    // 240 ms after arming, before any later upload could check it).
    return true;
  }

  upload_command_buffer_.clear();
  upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
  DkCopyBuf source_buffer{};
  source_buffer.addr = (*staging).getGpuAddr();
  // Run-061: the source descriptor, with the units deko3d actually uses.
  //
  // DkCopyBuf is Vulkan's VkBufferImageCopy pair in disguise: rowLength is the
  // BYTES per row and imageHeight is the ROW COUNT of the padded source (0 in
  // both means "tightly packed", which is what deko3d's own examples pass). The
  // port had been passing the TOTAL BYTE SIZE in imageHeight - 2,949,120 instead
  // of 720 for a 960x720 upload - so the 2D engine was told to walk 2.9 million
  // rows of a 2.8 MB block. That is invalid from the very first byte it runs
  // past the block, which is exactly why the app died the instant the copy
  // started executing (runs 042-048 only looked healthier because the upload was
  // skipped after its fence "timed out").
  source_buffer.rowLength = source_pitch;
  source_buffer.imageHeight = blocks_high;
  dk::ImageView destination_view{deko_texture.image()};
  destination_view.setMipLevels(0, 1);
  const DkImageRect destination_rect{0, 0, 0, width, height, 1};
  upload_command_buffer_.copyBufferToImage(source_buffer, destination_view, destination_rect);
  // Drop the GPU's cached image data afterwards: the draws that sample this
  // texture are recorded into a different list, so queue order alone does not
  // guarantee they see the new texels.
  upload_command_buffer_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
  DkCmdList list = upload_command_buffer_.finishList();
  // Presenter-style submit: submit, do not wait, use the fence only to know when
  // the staging block can be recycled. Queue order orders the copy before every
  // draw that samples the image, so no wait is needed for correctness. (The old
  // "the copy's fence never signals" premise is retracted - run 068: fences
  // signal in ~5 ms once the submission is kicked; the timeouts were unfetched
  // GPFIFO entries, not dead fences.)
  static std::vector<std::unique_ptr<dk::UniqueMemBlock>> deferred_staging;
  static std::vector<std::unique_ptr<dk::Fence>> deferred_fences;
  // Submit epoch of each block's copy (run-060 age-based retirement).
  static std::vector<uint64_t> deferred_epochs;
  static std::vector<uint64_t> deferred_present_epochs;
  // Bounded so a session where fences never signal cannot grow without limit;
  // each block is one texture's staging size.
  constexpr size_t kMaxDeferred = 64;

  if (!REXCVAR_GET(deko3d_upload_no_wait)) {
    std::lock_guard lock(device_.submit_mutex());
    device_.queue().submitCommands(list);
    device_.queue().signalFence(upload_fence_, true);
    if (!WaitFence(upload_fence_, "texture upload", upload_fence_stats_)) {
      upload_stopped_ = true;
      REXGPU_WARN("deko3d: texture upload stalled; further textures will not be "
                  "uploaded and will sample the placeholder instead");
      return false;
    }
    REXGPU_INFO("deko3d: uploaded texture {} ({}x{}, format {})", key.GetLogDimensionName(),
                width, height, uint32_t(key.format));
    return true;
  }

  // Run-069: RunCopyDiagnostic (the run-065 a-d standalone probes) is retired -
  // its FAIL verdicts were the missing-kickoff artefact, corrected by run 068's
  // EXECUTED. The function stays for opt-in A/B only; nothing calls it by default.

  // Retire by FENCE (primary - run 068 proved fences signal once kicked), by
  // present-age (the queue is in-order, so two presents past the copy's frame
  // mean it executed), or by submit-age as the last-resort bound.
  constexpr uint64_t kStagingRetireEpochs = 64;  // last resort only
  static FenceWaitStats retire_fence_stats;
  const uint64_t now_present = PresentEpoch();
  // Acquire before looking at (or reusing) any block the GPU may have read.
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint64_t now_epoch = GpuSubmitEpoch();
  for (size_t i = deferred_fences.size(); i-- > 0;) {
    // The copy's own fence is primary (it signals once the submit is kicked);
    // the present age is the in-order witness: once two frames later have been
    // presented, the copy has executed and the block can be freed. Age 64 is only
    // a last-resort bound so the list cannot grow without limit.
    const bool fence_done =
        deferred_fences[i] &&
        PollFence(*deferred_fences[i], "texture staging", retire_fence_stats);
    const bool witnessed = now_present >= deferred_present_epochs[i] &&
                           (now_present - deferred_present_epochs[i]) >= 2;
    const bool aged_out = now_epoch >= deferred_epochs[i] &&
                          (now_epoch - deferred_epochs[i]) >= kStagingRetireEpochs;
    if (fence_done || witnessed || aged_out) {
      deferred_fences.erase(deferred_fences.begin() + ptrdiff_t(i));
      deferred_staging.erase(deferred_staging.begin() + ptrdiff_t(i));
      deferred_epochs.erase(deferred_epochs.begin() + ptrdiff_t(i));
      deferred_present_epochs.erase(deferred_present_epochs.begin() + ptrdiff_t(i));
    }
  }
  if (deferred_staging.size() >= kMaxDeferred) {
    upload_stopped_ = true;
    REXGPU_WARN("deko3d: {} texture staging blocks still in flight (retirement not keeping "
                "up); further textures will sample the placeholder",
                uint32_t(deferred_staging.size()));
    return false;
  }

  auto fence = std::make_unique<dk::Fence>();
  {
    std::lock_guard lock(device_.submit_mutex());
    device_.queue().submitCommands(list);
    device_.queue().signalFence(*fence, true);
    // Run-069: kick the channel. submitCommands/signalFence only append GPFIFO
    // entries; without this flush the copy (and its fence) sit unfetched until
    // the next presentImage - the run-058/065/067 "fence never signals" artefact.
    device_.queue().flush();
  }
  deferred_staging.push_back(std::move(staging));
  deferred_fences.push_back(std::move(fence));
  deferred_epochs.push_back(GpuSubmitEpoch());
  deferred_present_epochs.push_back(PresentEpoch());
  static uint32_t no_wait_logged = 0;
  if (no_wait_logged < 12) {
    ++no_wait_logged;
    REXGPU_WARN("deko3d: uploaded texture {} ({}x{}, format {}) without a fence wait; "
                "{} staging block(s) in flight, retired by age (8 submits)",
                key.GetLogDimensionName(), width, height, uint32_t(key.format),
                uint32_t(deferred_staging.size()));
  }
  return true;
}

bool Deko3DTextureCache::GetBoundTexture(uint32_t fetch_constant, bool want_signed,
                                         BoundTexture& out) {
  // Run-069: the frame-path probe is printed by PollFrameUploadProbe() from the
  // presenter (every present), which survives even when no draw binds a texture.
  out = BoundTexture();
  if (!REXCVAR_GET(deko3d_texture_upload)) {
    // No texture is ever uploaded, so bind the draw path's white placeholder for
    // all of them: the menu then renders untextured but with correct geometry,
    // blending and depth, which is what the draw path needs to be validated on.
    return false;
  }
  if (fetch_constant >= xenos::kTextureFetchConstantCount) {
    return false;
  }
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant);
  if (!binding) {
    return false;
  }
  Texture* texture = want_signed && binding->texture_signed ? binding->texture_signed
                                                            : binding->texture;
  if (!texture) {
    texture = binding->texture_signed ? binding->texture_signed : binding->texture;
  }
  if (!texture) {
    return false;
  }
  // Keep the texture alive while this frame's command list references its image.
  texture->MarkAsUsed();
  TextureImpl& deko_texture = static_cast<TextureImpl&>(*texture);
  if (!deko_texture.valid()) {
    return false;
  }
  out.image = &deko_texture.image();
  out.image_type = deko_texture.image_type();
  out.width = deko_texture.width();
  out.height = deko_texture.height();
  out.depth_or_array_size = texture->key().depth_or_array_size_minus_1 + 1;
  out.dimension = texture->key().dimension;
  return true;
}

}  // namespace rex::graphics::deko3d
