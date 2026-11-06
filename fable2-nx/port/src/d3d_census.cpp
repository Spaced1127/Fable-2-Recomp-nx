// Fable II D3D-library call census - implementation.
//
// Zero-allocation hot path: one atomic increment plus first-call sampling of
// the caller and argument registers. A detached reporter thread (started on
// the first hooked call) writes a sorted summary every 5 seconds.

#include "d3d_census.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

namespace fable2::census {

// Defined by the generated hooks file.
extern const uint32_t kFunctionCount;
extern const uint32_t kFunctionAddr[];
extern const char* const kFunctionName[];

namespace {

constexpr uint32_t kMaxFunctions = 4096;
constexpr int kArgs = 5;  // r3..r7

std::atomic<uint64_t> g_calls[kMaxFunctions];
std::atomic<uint64_t> g_total[kMaxFunctions];
std::atomic<uint32_t> g_caller[kMaxFunctions];
std::atomic<uint32_t> g_arg_min[kMaxFunctions][kArgs];
std::atomic<uint32_t> g_arg_max[kMaxFunctions][kArgs];
std::atomic<bool> g_started{false};
std::chrono::steady_clock::time_point g_t0;

void TouchArg(uint32_t index, int k, uint32_t v, bool first) {
  if (first) {
    g_arg_min[index][k].store(v, std::memory_order_relaxed);
    g_arg_max[index][k].store(v, std::memory_order_relaxed);
    return;
  }
  uint32_t cur = g_arg_min[index][k].load(std::memory_order_relaxed);
  while (v < cur && !g_arg_min[index][k].compare_exchange_weak(cur, v)) {
  }
  cur = g_arg_max[index][k].load(std::memory_order_relaxed);
  while (v > cur && !g_arg_max[index][k].compare_exchange_weak(cur, v)) {
  }
}

void Report() {
  static uint64_t last[kMaxFunctions] = {};
  const size_t n = kFunctionCount < kMaxFunctions ? kFunctionCount : kMaxFunctions;
  struct Row {
    uint64_t delta;
    uint32_t index;
  };
  std::vector<Row> rows;
  rows.reserve(n);
  uint64_t sum = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t total = g_calls[i].load(std::memory_order_relaxed);
    const uint64_t delta = total - last[i];
    last[i] = total;
    if (delta) {
      rows.push_back({delta, i});
      sum += delta;
    }
  }
  if (rows.empty()) {
    return;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.delta != b.delta ? a.delta > b.delta : a.index < b.index;
  });
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  REXLOG_INFO("[d3d-census] t={:.1f}s: {} function(s), {} call(s)", secs, rows.size(), sum);
  const size_t cap = rows.size() < 220 ? rows.size() : 220;
  for (size_t r = 0; r < cap; ++r) {
    const uint32_t i = rows[r].index;
    REXLOG_INFO(
        "[d3d-census]   {:08X} {:<44} d={:<7} tot={:<9} lr={:08X} "
        "r3={:X}..{:X} r4={:X}..{:X} r5={:X}..{:X} r6={:X}..{:X} r7={:X}..{:X}",
        kFunctionAddr[i], kFunctionName[i], rows[r].delta,
        (unsigned long long)g_calls[i].load(std::memory_order_relaxed),
        g_caller[i].load(std::memory_order_relaxed),
        g_arg_min[i][0].load(std::memory_order_relaxed),
        g_arg_max[i][0].load(std::memory_order_relaxed),
        g_arg_min[i][1].load(std::memory_order_relaxed),
        g_arg_max[i][1].load(std::memory_order_relaxed),
        g_arg_min[i][2].load(std::memory_order_relaxed),
        g_arg_max[i][2].load(std::memory_order_relaxed),
        g_arg_min[i][3].load(std::memory_order_relaxed),
        g_arg_max[i][3].load(std::memory_order_relaxed),
        g_arg_min[i][4].load(std::memory_order_relaxed),
        g_arg_max[i][4].load(std::memory_order_relaxed));
  }
  if (rows.size() > cap) {
    REXLOG_INFO("[d3d-census]   ... {} more function(s) not shown", rows.size() - cap);
  }
}

void ReporterMain() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    Report();
  }
}

}  // namespace

void OnCall(uint32_t index, const PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (index >= kMaxFunctions) {
    return;
  }
  const uint64_t total =
      g_total[index].fetch_add(1, std::memory_order_relaxed) + 1;
  g_calls[index].store(total, std::memory_order_relaxed);
  if (total == 1) {
    g_caller[index].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  }
  const bool first = total == 1;
  TouchArg(index, 0, ctx.r3.u32, first);
  TouchArg(index, 1, ctx.r4.u32, first);
  TouchArg(index, 2, ctx.r5.u32, first);
  TouchArg(index, 3, ctx.r6.u32, first);
  TouchArg(index, 4, ctx.r7.u32, first);
  if (!g_started.exchange(true, std::memory_order_relaxed)) {
    g_t0 = std::chrono::steady_clock::now();
    std::thread(ReporterMain).detach();
    REXLOG_INFO("[d3d-census] reporter started ({} functions hooked)", kFunctionCount);
  }
}

}  // namespace fable2::census
