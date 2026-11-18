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

// Run-063 CORRECTION: fences on this stack DO signal - run 062's present-copy
// fence signaled one frame after submission, on the render queue. The earlier
// claim that "fences never signal" was wrong: it came from a probe whose list had
// already been overtaken by an invalid 2D copy (and from a boot-time test on a
// queue that had never executed anything). What remains true and useful is that
// every wait must be BOUNDED and named, because a wait on a wedged queue stops the
// command processor and the game with it.
constexpr int64_t kFenceWaitNs = 5000000000;

// The bound WaitFence actually uses, in nanoseconds (cvar deko3d_fence_wait_ms).
int64_t FenceWaitTimeoutNs();

// Accumulated state for one fence, so a stall is named the first time and then
// rate-limited rather than flooding the log every frame.
struct FenceWaitStats {
  uint64_t timeouts = 0;
  uint64_t observed = 0;
};

// Waits for a fence with FenceWaitTimeoutNs(). Returns true if it signaled. On a
// timeout, logs `what` once and then every 32nd occurrence.
bool WaitFence(dk::Fence& fence, const char* what, FenceWaitStats& stats);

// Run-059: polls a fence once and never blocks.
//
// The run-058 self-test settled it: a fence signalled on an EMPTY command list
// (`fence-only`) also times out, so dkFenceWait never observes a signal on this
// stack at all. Every wait therefore has to become a poll - a wait can only cost
// time now, and at 250 ms per use it was still throttling the display to a
// standstill.
bool PollFence(dk::Fence& fence, const char* what, FenceWaitStats& stats);

// Run-063: bounded wait used by resource retirement (250 ms), returns true if it
// signalled and records the observation for FencesObserved().
bool WaitFenceForRetirement(dk::Fence& fence, const char* what, FenceWaitStats& stats);

// Run-060/063: the fallback for resource lifetime.
//
// Run 063 restored fences as the PRIMARY mechanism (they work - see
// WaitFenceForRetirement). The submit epoch stays as the fallback for the case
// where a fence does not report: every submit bumps it, and because the queue
// executes in submission order, a resource whose last reader was submitted at
// epoch E is finished once a few later epochs have gone through. Age is a
// heuristic; it must never replace a fence that is known to work.
void NoteGpuSubmit();
uint64_t GpuSubmitEpoch();

// Run-063: fences DO signal on this stack - run 062 observed the present copy's
// fence signing on the render queue one frame after submission. The old
// "fences never signal" note is deleted; what remains true is that they must be
// BOUNDED and named. This flag records that a fence has actually been observed,
// so resource retirement can prefer a short real wait and fall back to age only
// when the primitive has not been seen to work.
void NoteFenceObserved();
bool FencesObserved();

// Run-064: the present fence is the one fence observed to work (run 062/063:
// signalled one frame after submission, on the render queue). The queue is
// in-order, so once the present fence for frame K has signalled, every list
// submitted before it - including our buffer->image copies - has completed. That
// makes it the witness for copy completion when a copy's own fence does not
// report. NoteFramePresented() is called by the presenter whenever its fence
// poll succeeds; PresentEpoch() counts those frames.
void NoteFramePresented();
uint64_t PresentEpoch();

}  // namespace rex::graphics::deko3d
