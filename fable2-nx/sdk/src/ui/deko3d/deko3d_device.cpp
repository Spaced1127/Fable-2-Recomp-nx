/**
 * @file        ui/deko3d/deko3d_device.cpp
 * @brief       Shared deko3d device and graphics queue.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <malloc.h>
#include <unistd.h>

#include <switch.h>

#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>

extern "C" char* fake_heap_end;  // libnx: the heap's end (sbrk grows towards it)

namespace rex::ui::deko3d {

namespace {

// How much memory is left, reported the way plume's deko3d backend does it:
// what malloc still holds free plus what sbrk can still take, and what the
// process' Horizon memory pool has unused. The Switch port runs close to its
// pool limit (the guest memory mirror alone is 512 MB), so every deko3d
// allocation has to be attributable.
void GetMemoryFigures(uint64_t& heap_free_out, uint64_t& process_free_out) {
  struct mallinfo info = mallinfo();
  char* break_address = static_cast<char*>(sbrk(0));
  heap_free_out = uint64_t(info.fordblks) +
                  (fake_heap_end > break_address ? uint64_t(fake_heap_end - break_address) : 0);
  uint64_t total = 0;
  uint64_t used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  process_free_out = total > used ? total - used : 0;
}

void LogMemoryFigures(const char* where) {
  uint64_t heap_free = 0;
  uint64_t process_free = 0;
  GetMemoryFigures(heap_free, process_free);
  REXLOG_INFO("[deko3d] {}: heap {} MiB free, process {} MiB unused of its pool", where,
              heap_free >> 20, process_free >> 20);
  // The Switch log lives on the SD and info lines reach it once a second, so a
  // process that dies during initialization loses them. Startup memory figures
  // are only worth having if they survive that.
  FlushLogging();
}

}  // namespace

std::unique_ptr<Deko3DDevice> Deko3DDevice::Create() {
  std::unique_ptr<Deko3DDevice> device(new Deko3DDevice());
  LogMemoryFigures("device: creating");
  device->device_ = dk::DeviceMaker{}.create();
  if (!device->device_) {
    REXLOG_ERROR("[deko3d] device creation failed");
    FlushLogging();
    return nullptr;
  }
  // Upper-left origin and 0...1 depth are deko3d's defaults, and also Vulkan's
  // and Direct3D's conventions - what Xenia's translated SPIR-V and
  // draw_util::GetHostViewportInfo already produce, so no viewport flip is
  // needed anywhere in the backend.
  //
  // Zcull off: SVR2011's deko3d backend found the GPU dropping depth-biased
  // decals in whole 4x8 tiles with it enabled, and this backend uses depth bias
  // for the guest's polygon offset.
  device->queue_ = dk::QueueMaker{device->device_}
                       .setFlags(DkQueueFlags_Graphics | DkQueueFlags_DisableZcull)
                       .setPerWarpScratchMemorySize(kPerWarpScratchSize)
                       .create();
  if (!device->queue_) {
    REXLOG_ERROR("[deko3d] graphics queue creation failed");
    FlushLogging();
    return nullptr;
  }
  LogMemoryFigures("graphics queue created");
  return device;
}

Deko3DDevice::~Deko3DDevice() {
  if (queue_) {
    queue_.waitIdle();
  }
}

void Deko3DDevice::WaitIdle() {
  std::lock_guard lock(submit_mutex_);
  if (queue_) {
    queue_.waitIdle();
  }
}

void Deko3DDevice::LogMemory(const char* where) const {
  LogMemoryFigures(where);
}

}  // namespace rex::ui::deko3d
