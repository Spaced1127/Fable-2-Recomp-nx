/**
 * @file        graphics/deko3d/fence_wait.h
 * @brief       Bounded waits for deko3d fences on this port.
 *
 * Both failure modes this backend has already measured on hardware end in a
 * fence that never signals: a 2D-engine copy out of a render target into the
 * guest mirror stalls the queue with `queue error state 0`, and a command list
 * submitted on a second graphics queue is simply never executed. The SVR2011
 * porting guide's rule is the same one - never `dkQueueWaitIdle`, use a fence,
 * and give the presentation copy a timeout.
 *
 * An unbounded wait on this port does not produce a crash; it produces a game
 * that is alive but stopped, because the thread that waits is the Xenos command
 * processor, so nothing ever swaps and the audio sink starves. Every wait for
 * GPU work in the deko3d backend is therefore bounded, and a timeout says which
 * fence timed out instead of silently hanging.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <deko3d.hpp>

namespace rex::graphics::deko3d {

// Long enough that a healthy but busy GPU never trips it, short enough that a
// stall is reported while the console is still responsive.
constexpr int64_t kFenceWaitNs = 5000000000;

// Accumulated state for one fence, so a stall is named the first time and then
// rate-limited rather than flooding the log every frame.
struct FenceWaitStats {
  uint64_t timeouts = 0;
};

// Waits for a fence with kFenceWaitNs. Returns true if it signaled. On a
// timeout, logs `what` once and then every 32nd occurrence.
bool WaitFence(dk::Fence& fence, const char* what, FenceWaitStats& stats);

}  // namespace rex::graphics::deko3d
