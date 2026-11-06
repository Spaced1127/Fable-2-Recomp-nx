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
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/deko3d/deko3d_presenter.h>
#include <rex/ui/presenter.h>

#if REX_PLATFORM_SWITCH
// The profiler's guest frame counter (switch_perf.cpp).
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

namespace rex::graphics::deko3d {

Deko3DCommandProcessor::Deko3DCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

Deko3DCommandProcessor::~Deko3DCommandProcessor() = default;

bool Deko3DCommandProcessor::SetupContext() {
  return true;
}

void Deko3DCommandProcessor::ShutdownContext() {}

void Deko3DCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
#if REX_PLATFORM_SWITCH
  // The profiler's guest frame counter (the Vulkan path counts here too).
  RexSwitchPerfCount(0);
#endif
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
  const bool refreshed = presenter->RefreshGuestOutput(
      display_width, display_height, display_width, display_height,
      [guest_pixels, display_width, display_height, guest_pitch](
          ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& deko3d_context =
            static_cast<ui::deko3d::Deko3DGuestOutputRefreshContext&>(context);
        deko3d_context.SetGuestOutput(guest_pixels, display_width, display_height,
                                      guest_pitch);
        context.SetIs8bpc(true);
        return guest_pixels != nullptr;
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
  return true;
}

}  // namespace rex::graphics::deko3d
