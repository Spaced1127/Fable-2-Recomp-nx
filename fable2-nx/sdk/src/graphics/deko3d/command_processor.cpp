/**
 * @file        graphics/deko3d/command_processor.cpp
 * @brief       CommandProcessor for the deko3d backend: brings the guest ring
 *              up and presents. Draws/copies are no-ops until the deko3d
 *              render target and texture caches are implemented.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/deko3d/command_processor.h>
#include <rex/graphics/deko3d/graphics_system.h>
#include <rex/graphics/deko3d/render_target_cache.h>
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

void Deko3DCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
#if REX_PLATFORM_SWITCH
  // The profiler's guest frame counter (the Vulkan path counts here too).
  RexSwitchPerfCount(0);
#endif
  // Submit this frame's clears and resolves before the presenter copies the
  // front buffer to the swapchain.
  if (render_target_cache_) {
    render_target_cache_->Flush();
  }
  if (!graphics_system_) {
    return;
  }
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    return;
  }
  const uint32_t display_width = frontbuffer_width ? frontbuffer_width : 1280;
  const uint32_t display_height = frontbuffer_height ? frontbuffer_height : 720;
  const uint8_t* guest_pixels = nullptr;
  // Direct CPU reads of the guest window are only safe for committed guest
  // *physical* memory: a fault in another heap is unhandled by the runtime
  // (observed with the front buffer before any GPU resolve has written it).
  // Until the guest buffer exists, the presenter keeps showing the pattern.
  if (graphics_system_ && graphics_system_->memory() && frontbuffer_ptr) {
    rex::memory::Memory* memory = graphics_system_->memory();
    rex::memory::BaseHeap* heap = memory->LookupHeap(frontbuffer_ptr);
    if (heap && heap->heap_type() == rex::memory::HeapType::kGuestPhysical) {
      rex::memory::HeapAllocationInfo info{};
      if (heap->QueryRegionInfo(frontbuffer_ptr, &info) &&
          (info.state & rex::memory::kMemoryAllocationCommit)) {
        guest_pixels =
            memory->TranslateVirtual<const uint8_t*>(frontbuffer_ptr);
      }
    }
  }
  const uint32_t guest_pitch = display_width * 4;
  DkGpuAddr guest_gpu_pixels = 0;
  if (shared_memory_ && frontbuffer_ptr) {
    const uint32_t physical = memory_->GetPhysicalAddress(frontbuffer_ptr);
    const uint64_t offset = physical;
    const uint64_t byte_size = uint64_t(display_width) * display_height * 4;
    if (offset + byte_size <= SharedMemory::kBufferSize) {
      guest_gpu_pixels = shared_memory_->gpu_address() + offset;
    }
  }
  const bool refreshed = presenter->RefreshGuestOutput(
      display_width, display_height, display_width, display_height,
      [guest_pixels, guest_gpu_pixels, display_width, display_height,
       guest_pitch](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& deko3d_context =
            static_cast<ui::deko3d::Deko3DGuestOutputRefreshContext&>(context);
        if (guest_gpu_pixels) {
          deko3d_context.SetGuestOutputGpu(guest_gpu_pixels, display_width, display_height,
                                           guest_pitch);
        } else {
          deko3d_context.SetGuestOutput(guest_pixels, display_width, display_height,
                                        guest_pitch);
        }
        context.SetIs8bpc(true);
        return guest_gpu_pixels != 0 || guest_pixels != nullptr;
      });
  (void)refreshed;
}

Shader* Deko3DCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  (void)shader_type;
  (void)guest_address;
  (void)host_address;
  (void)dword_count;
  // Shader translation to DKSH comes with the draw path (D2).
  return nullptr;
}

bool Deko3DCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  return true;
}

bool Deko3DCommandProcessor::IssueCopy() {
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
