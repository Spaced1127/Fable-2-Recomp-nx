/**
 * @file        rex/graphics/deko3d/upload_math.h
 * @brief       Pure upload geometry/lifetime math, host-testable.
 *
 * Run-079: extracted from texture_cache.cpp so the rules the batch relies on
 * can be verified WITHOUT a device (sdk/tests/upload_math_test.cpp, compiled
 * and run on the host). Behavior is identical to the inline code it replaces;
 * every constant here is tied to recorded hardware evidence:
 *
 *  - w64/h8 alignment: run-074/075 layout hygiene (deko3d pads tiled layouts;
 *    NOT the fault cause - retracted, kept harmless).
 *  - 4096-byte source pitch: the proven-good pitch (runs 067/068/076 matrix).
 *  - cohort release / bind-allowed: run-070 staging lifetime + run-073
 *    one-frame sampling delay, on the in-order-queue argument (a later frame's
 *    present fence signalled => earlier lists completed).
 *  - prologue cap fit: run-078 overflow guard (deko3d cmdbufs do not
 *    bounds-check).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex::graphics::deko3d::upload_math {

// AlignUp for power-of-two alignments.
constexpr uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Run-075 image extent (2D): width to 64 px, height to 8 (GOB) rows.
constexpr uint32_t AlignedWidth(uint32_t w) { return AlignUp(w, 64); }
constexpr uint32_t AlignedHeight(uint32_t h) { return AlignUp(h, 8); }

// Source row pitch: 4096-byte multiple (the proven-good pitch).
constexpr uint32_t SourcePitch(uint32_t aligned_row_bytes) { return AlignUp(aligned_row_bytes, 4096); }

// Staging covers the ALIGNED rows; decode fills logical rows, memset zeroes the
// padding tail and extra rows.
constexpr uint32_t StagingBytes(uint32_t source_pitch, uint32_t aligned_blocks_high) {
  return source_pitch * aligned_blocks_high;
}

// deko3d memblocks are whole pages.
constexpr uint32_t PageRound(uint32_t v) { return AlignUp(v, 0x1000); }

// Run-078: the prologue cap must fit the command block at a conservative
// per-copy budget (copyBufferToImage + Full barrier is well under 512 bytes).
constexpr bool PrologueCapFits(uint32_t cap, uint32_t per_copy_budget, uint32_t block_bytes) {
  return uint64_t(cap) * per_copy_budget <= block_bytes;
}

// Run-070: a staging cohort is released only when a LATER frame's present fence
// has signalled (in-order queue => the cohort's copies completed). Never age.
constexpr bool CohortReleased(uint64_t present_epoch, uint64_t cohort) {
  return present_epoch >= cohort + 1;
}

// Run-073/077: an uploaded image binds only once its copy's frame has been
// submitted (one-frame delay); UINT64_MAX = depth-skipped, NEVER bind;
// cohort 0 = never uploaded -> placeholder while the delay is on.
constexpr bool BindAllowed(uint64_t frame_submit_epoch, uint64_t cohort, bool defer_sampling) {
  if (cohort == UINT64_MAX) {
    return false;
  }
  if (!defer_sampling) {
    return true;  // legacy A/B path
  }
  return cohort != 0 && frame_submit_epoch >= cohort;
}

// Compile-time guards (run on host AND target builds).
static_assert(AlignedWidth(480) == 512 && AlignedWidth(240) == 256, "w64 alignment");
static_assert(AlignedWidth(960) == 960, "already-aligned width unchanged");
static_assert(AlignedHeight(180) == 184 && AlignedHeight(360) == 360, "h8 alignment");
static_assert(SourcePitch(2048) == 4096 && SourcePitch(1024) == 4096, "4096 pitch");
static_assert(StagingBytes(4096, 360) == 1474560, "staging extent");
static_assert(PrologueCapFits(128, 512, 64 << 10), "prologue cap fits 64 KB");
static_assert(!PrologueCapFits(129, 512, 64 << 10), "cap+1 must not fit");
static_assert(CohortReleased(5, 4) && !CohortReleased(4, 4), "cohort needs a LATER fence");
static_assert(!BindAllowed(10, UINT64_MAX, true), "depth-skipped never binds");
static_assert(!BindAllowed(10, 0, true), "never-uploaded binds placeholder");
static_assert(BindAllowed(4, 4, true) && !BindAllowed(3, 4, true), "one-frame delay");

}  // namespace rex::graphics::deko3d::upload_math
