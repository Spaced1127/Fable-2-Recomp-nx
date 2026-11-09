/**
 * @file        graphics/deko3d/fence_wait.cpp
 * @brief       Bounded waits for deko3d fences on this port.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/deko3d/fence_wait.h>
#include <rex/logging.h>

namespace rex::graphics::deko3d {

bool WaitFence(dk::Fence& fence, const char* what, FenceWaitStats& stats) {
  const DkResult result = fence.wait(kFenceWaitNs);
  if (result == DkResult_Success) {
    return true;
  }
  ++stats.timeouts;
  // Named once, then rate-limited: a fence that never signals is a permanent
  // condition, and the wait is on the thread that drives the guest ring, so the
  // caller has to be told which one it is without flooding the card.
  if (stats.timeouts == 1 || (stats.timeouts % 32) == 0) {
    REXGPU_WARN("deko3d: fence wait timed out for {} after {} ms (occurrence {})", what,
                kFenceWaitNs / 1000000, stats.timeouts);
  }
  return false;
}

}  // namespace rex::graphics::deko3d
