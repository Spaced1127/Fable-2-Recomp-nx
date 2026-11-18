/**
 * @file        graphics/deko3d/fence_wait.h
 * @brief       Bounded waits for deko3d fences on this port.
 *
 * RUN-068 CORRECTION (hardware-proven): fences on this stack DO signal, in
 * ~5 ms, once the submission is KICKED. The installed deko3d's submitCommands
 * and signalFence only append GPFIFO entries to a pending batch; the doorbell
 * (nvGpuChannelKickoff) fires only from Queue::flush - reached via presentImage
 * in the frame loop, via waitIdle, or an explicit flush(). Every historical
 * "fence never signals" / "copy is the killer" / "standalone submit path is
 * dead" verdict (runs 057/058/065/067) was an UNFETCHED submission, not hardware
 * behaviour: the probes waited without anything kicking the channel, and 065's
 * probe even blocked the presenting thread inside its waits. Do not re-derive
 * those conclusions; a standalone submit that must complete needs flush() (or
 * waitIdle) after signalFence.
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

// Fences signal once kicked (run 068: waitIdle returned in 5 ms, fence
// signalled, queue error state no, readback = the pattern). Every wait stays
// BOUNDED and named, because a wait on a wedged queue stops the command
// processor and the game with it.
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

// Polls a fence once and never blocks. Use where a wait would stall the frame
// loop and lateness is harmless (telemetry, opportunistic retirement). For a
// submission that must COMPLETE, prefer a bounded WaitFence after an explicit
// queue.flush() - the run-058 "fence-only times out" verdict that justified
// poll-everywhere was the missing-kickoff artefact (run 068 correction).
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

// The queue is in-order, so once the present fence for frame K has signalled,
// every list submitted before it - including our buffer->image copies - has
// completed. That makes the present age a witness for copy completion alongside
// the copy's own fence (which signals once kicked - run 068).
// NoteFramePresented() is called by the presenter whenever its fence wait/poll
// succeeds; PresentEpoch() counts those frames.
void NoteFramePresented();
uint64_t PresentEpoch();

// Run-069 death-watch: the ~9.5 s below-guest death writes no crash block, so the
// log tail must name the last stage. NoteTextureUpload() bumps a counter the
// presenter's periodic [death-watch] line prints with the present count.
void NoteTextureUpload();
uint64_t TextureUploadCount();

// Run-069: print the frame-path upload probe's readback (non-blocking). Called by
// the presenter on every present so a death shortly after the first upload cannot
// pre-empt the pixels (run 068: armed, then death 240 ms later, nothing printed).
void PollFrameUploadProbe();

}  // namespace rex::graphics::deko3d
