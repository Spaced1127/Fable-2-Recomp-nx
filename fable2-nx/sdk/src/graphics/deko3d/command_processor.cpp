/**
 * @file        graphics/deko3d/command_processor.cpp
 * @brief       CommandProcessor for the deko3d backend: brings the guest ring
 *              up and presents. Draws/copies are no-ops until the deko3d
 *              render target and texture caches are implemented.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdio>
#include <set>
#include <string>

#include <xxhash.h>

#include <rex/graphics/deko3d/command_processor.h>
#include <rex/graphics/deko3d/graphics_system.h>
#include <rex/graphics/deko3d/render_target_cache.h>
#include <rex/graphics/deko3d/shader_pack.h>
#include <rex/graphics/deko3d/shared_memory.h>
#include <rex/graphics/deko3d/texture_cache.h>
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
  shared_memory_ = std::make_unique<Deko3DSharedMemory>(*device_, *memory_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("deko3d: failed to initialize shared memory");
    shared_memory_.reset();
    return false;
  }
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
  return true;
}

void Deko3DCommandProcessor::ShutdownContext() {
  if (texture_cache_) {
    texture_cache_->Shutdown();
    texture_cache_.reset();
  }
  if (render_target_cache_) {
    render_target_cache_->Flush();
    render_target_cache_->Shutdown();
    render_target_cache_.reset();
  }
  if (device_) {
    device_->WaitIdle();
  }
  shared_memory_.reset();
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

void Deko3DCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
#if REX_PLATFORM_SWITCH
  // The profiler's guest frame counter (the Vulkan path counts here too).
  RexSwitchPerfCount(0);
#endif
  // Submit this frame's clears and resolves before the presenter copies the
  // front buffer to the swapchain.
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
    REXGPU_INFO(
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

Shader* Deko3DCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  if (!shader_pack_ || !host_address || !dword_count) {
    return nullptr;
  }
  // Same hash the dump_shaders aid uses for its file names, so the pack keys
  // (ps_<hash>_<modification>) match directly.
  const uint64_t ucode_hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  char prefix[24];
  std::snprintf(prefix, sizeof(prefix), "%s_%016llX",
                shader_type == xenos::ShaderType::kPixel ? "ps" : "vs",
                static_cast<unsigned long long>(ucode_hash));

  uint32_t dksh_size = 0;
  std::string pack_key;
  const uint8_t* dksh = shader_pack_->FindPrefix(prefix, dksh_size, &pack_key);

  // Called only from the command processor thread.
  static std::set<std::string> logged_keys;
  static uint32_t found_count = 0;
  static uint32_t missing_count = 0;
  if (dksh) {
    ++found_count;
  } else {
    ++missing_count;
  }
  if (logged_keys.insert(prefix).second) {
    if (dksh) {
      REXGPU_INFO("deko3d: shader {} found in pack as {} ({} bytes)", prefix, pack_key,
                  dksh_size);
    } else {
      REXGPU_WARN("deko3d: shader {} is NOT in the pack (draws using it are skipped)",
                  prefix);
    }
    if ((found_count + missing_count) % 100 == 0) {
      REXGPU_INFO("deko3d: shader lookups so far: {} in pack, {} missing", found_count,
                  missing_count);
    }
  }
  // Pipeline creation from the DKSH lands with the draw path; until then the
  // draw is skipped and the lookup above tells us whether the pack is complete.
  return nullptr;
}

bool Deko3DCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  // Resolves are submitted as draw packets with EDRAM copy mode (the Vulkan and
  // D3D12 backends dispatch the same way). Without this check the guest's
  // resolve draws are silently dropped.
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
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

}  // namespace rex::graphics::deko3d
