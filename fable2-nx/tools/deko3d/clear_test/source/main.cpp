// deko3d bring-up test for the Fable II Switch port.
//
// Creates a deko3d device and queue, two presentable RGBA8 framebuffers, a
// swapchain on the default libnx NWindow, and clears a static colour pattern
// every frame. This is the device/swapchain/present code the SDK's deko3d
// presenter will use; if this shows on the console, deko3d presentation works.

#include <switch.h>
#include <deko3d.hpp>

#include <array>
#include <cstdio>

namespace {

constexpr unsigned kNumFramebuffers = 2;
constexpr uint32_t kFramebufferWidth = 1280;
constexpr uint32_t kFramebufferHeight = 720;
constexpr unsigned kStaticCmdSize = 0x1000;

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  dk::UniqueDevice device = dk::DeviceMaker{}.create();
  dk::UniqueQueue queue =
      dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();

  // Image memory for the framebuffers.
  dk::ImageLayout layout_framebuffer;
  dk::ImageLayoutMaker{device}
      .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                DkImageFlags_HwCompression)
      .setFormat(DkImageFormat_RGBA8_Unorm)
      .setDimensions(kFramebufferWidth, kFramebufferHeight)
      .initialize(layout_framebuffer);

  const uint64_t fb_size = layout_framebuffer.getSize();
  const uint32_t fb_align = layout_framebuffer.getAlignment();
  const uint64_t fb_stride = (fb_size + fb_align - 1) & ~uint64_t(fb_align - 1);
  dk::UniqueMemBlock fb_mem =
      dk::MemBlockMaker{device, uint32_t(fb_stride * kNumFramebuffers)}
          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
          .create();

  std::array<dk::Image, kNumFramebuffers> framebuffers;
  std::array<DkImage const*, kNumFramebuffers> fb_array;
  for (unsigned i = 0; i < kNumFramebuffers; ++i) {
    framebuffers[i].initialize(layout_framebuffer, fb_mem, i * fb_stride);
    fb_array[i] = &framebuffers[i];
  }

  // Command buffer memory and the static command lists.
  dk::UniqueMemBlock data_mem =
      dk::MemBlockMaker{device, kStaticCmdSize * (kNumFramebuffers + 1)}
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
  dk::UniqueCmdBuf cmdbuf = dk::CmdBufMaker{device}.create();

  std::array<DkCmdList, kNumFramebuffers> bind_cmdlists;
  for (unsigned i = 0; i < kNumFramebuffers; ++i) {
    cmdbuf.addMemory(data_mem, i * kStaticCmdSize, kStaticCmdSize);
    dk::ImageView color_target{framebuffers[i]};
    cmdbuf.bindRenderTargets(&color_target);
    bind_cmdlists[i] = cmdbuf.finishList();
  }

  // A recognisable static pattern: green background with a light box and a
  // dark green cross, so it cannot be confused with a black/blank screen.
  cmdbuf.addMemory(data_mem, kNumFramebuffers * kStaticCmdSize, kStaticCmdSize);
  cmdbuf.setScissors(0, {{0, 0, kFramebufferWidth, kFramebufferHeight}});
  cmdbuf.clearColor(0, DkColorMask_RGBA, 0.0f, 0.25f, 0.0f, 1.0f);
  const unsigned box = 400;
  const unsigned box_x = kFramebufferWidth / 2 - box / 2;
  const unsigned box_y = kFramebufferHeight / 2 - box / 2;
  cmdbuf.setScissors(0, {{box_x, box_y, box, box}});
  cmdbuf.clearColor(0, DkColorMask_RGBA, 229 / 255.0f, 1.0f, 232 / 255.0f, 1.0f);
  cmdbuf.setScissors(0, {{box_x + box / 5, box_y + box / 2, box * 3 / 5, box / 4}});
  cmdbuf.clearColor(0, DkColorMask_RGBA, 0.0f, 0.5f, 0.0f, 1.0f);
  DkCmdList pattern_cmdlist = cmdbuf.finishList();

  dk::UniqueSwapchain swapchain =
      dk::SwapchainMaker{device, nwindowGetDefault(), fb_array}.create();

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
      break;
    }

    const int slot = queue.acquireImage(swapchain);
    queue.submitCommands(bind_cmdlists[slot]);
    queue.submitCommands(pattern_cmdlist);
    queue.presentImage(swapchain, slot);
  }

  queue.waitIdle();
  swapchain.destroy();
  cmdbuf.clear();
  return 0;
}
