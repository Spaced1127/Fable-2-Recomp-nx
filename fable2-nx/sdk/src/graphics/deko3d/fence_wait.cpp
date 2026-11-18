/**
 * @file        graphics/deko3d/fence_wait.cpp
 * @brief       Bounded waits for deko3d fences on this port.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/deko3d/fence_wait.h>
#include <atomic>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace rex::graphics::deko3d {

namespace {
std::atomic<uint64_t> g_submit_epoch{1};
std::atomic<bool> g_fence_observed{false};
std::atomic<uint64_t> g_present_epoch{0};
std::atomic<uint64_t> g_texture_uploads{0};
}  // namespace

void NoteGpuSubmit() { g_submit_epoch.fetch_add(1, std::memory_order_relaxed); }
void NoteFenceObserved() { g_fence_observed.store(true, std::memory_order_relaxed); }
bool FencesObserved() { return g_fence_observed.load(std::memory_order_relaxed); }
void NoteFramePresented() { g_present_epoch.fetch_add(1, std::memory_order_relaxed); }
uint64_t PresentEpoch() { return g_present_epoch.load(std::memory_order_relaxed); }
uint64_t GpuSubmitEpoch() { return g_submit_epoch.load(std::memory_order_relaxed); }
void NoteTextureUpload() { g_texture_uploads.fetch_add(1, std::memory_order_relaxed); }
uint64_t TextureUploadCount() { return g_texture_uploads.load(std::memory_order_relaxed); }

// How long any GPU wait may block. Fences signal in ~5 ms once the submission is
// kicked (run 068), so this bound is only about a *stuck* queue: 250 ms keeps a
// healthy GPU unaffected and turns a wedged one into a log line instead of a
// frozen game. Set 5000 for the old behaviour when A/B-ing.
REXCVAR_DEFINE_INT32(deko3d_fence_wait_ms, 250, "GPU/deko3d",
                     "Bounded GPU wait before a fence is treated as lost, in milliseconds "
                     "(default 250; 5000 is the pre-run-058 behaviour).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

int64_t FenceWaitTimeoutNs() {
  const int32_t ms = REXCVAR_GET(deko3d_fence_wait_ms);
  if (ms <= 0) {
    return kFenceWaitNs;
  }
  return int64_t(ms) * 1000000;
}

bool WaitFence(dk::Fence& fence, const char* what, FenceWaitStats& stats) {
  const int64_t timeout_ns = FenceWaitTimeoutNs();
  const DkResult result = fence.wait(timeout_ns);
  if (result == DkResult_Success) {
    NoteFenceObserved();
    return true;
  }
  ++stats.timeouts;
  // Named once, then rate-limited: a fence that never signals is a permanent
  // condition, and the wait is on the thread that drives the guest ring, so the
  // caller has to be told which one it is without flooding the card.
  if (stats.timeouts == 1 || (stats.timeouts % 32) == 0) {
    REXGPU_WARN("deko3d: fence wait timed out for {} after {} ms (occurrence {})", what,
                timeout_ns / 1000000, stats.timeouts);
  }
  return false;
}

bool PollFence(dk::Fence& fence, const char* what, FenceWaitStats& stats) {
  const DkResult result = fence.wait(0);
  if (result == DkResult_Success) {
    ++stats.observed;
    NoteFenceObserved();
    return true;
  }
  ++stats.timeouts;
  if (stats.timeouts == 1) {
    REXGPU_WARN("deko3d: {} fence not signalled on the first poll; continuing without a wait "
                "(fences signal once kicked - run 068; this one is simply not done yet)",
                what);
  } else if ((stats.timeouts % 512) == 0) {
    REXGPU_WARN("deko3d: {} fence still not signalled ({} polls)", what, stats.timeouts);
  }
  return false;
}

bool WaitFenceForRetirement(dk::Fence& fence, const char* what, FenceWaitStats& stats) {
  // Named, bounded, 250 ms: long enough for a 2D copy on a busy queue, short
  // enough that a lost signal costs a hiccup rather than a frozen frame.
  constexpr int64_t kRetireWaitNs = 250000000;
  if (fence.wait(kRetireWaitNs) == DkResult_Success) {
    NoteFenceObserved();
    ++stats.observed;
    return true;
  }
  ++stats.timeouts;
  if (stats.timeouts == 1 || (stats.timeouts % 64) == 0) {
    REXGPU_WARN("deko3d: {} fence not signalled within 250 ms (occurrence {}); falling back to "
                "age-based retirement",
                what, stats.timeouts);
  }
  return false;
}

}  // namespace rex::graphics::deko3d