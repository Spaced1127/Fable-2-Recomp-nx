/**
 * Host-side test for the batch's upload geometry/lifetime rules (run-079).
 * No device needed: compile and run with the host compiler, e.g. (WSL):
 *
 *   g++ -std=c++23 -I sdk/include sdk/tests/upload_math_test.cpp -o /tmp/upload_math_test
 *   /tmp/upload_math_test
 *
 * Every case is tied to recorded hardware evidence (see upload_math.h). The
 * same rules are static_asserted in the header, so target builds check them
 * too; this executable adds the runtime sweep over the real texture sizes.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/deko3d/upload_math.h>

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace rex::graphics::deko3d::upload_math;

static int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

int main() {
  // Alignment: the real GUI texture sizes from runs 074-076.
  CHECK(AlignedWidth(480) == 512);
  CHECK(AlignedWidth(240) == 256);
  CHECK(AlignedWidth(960) == 960);
  CHECK(AlignedWidth(1) == 64);
  CHECK(AlignedHeight(180) == 184);
  CHECK(AlignedHeight(360) == 360);
  CHECK(AlignedHeight(720) == 720);
  CHECK(AlignedHeight(1) == 8);
  // Idempotence: aligning an aligned value changes nothing.
  for (uint32_t w = 1; w <= 2048; ++w) {
    CHECK(AlignedWidth(AlignedWidth(w)) == AlignedWidth(w));
    CHECK(AlignedWidth(w) % 64 == 0);
    CHECK(AlignedWidth(w) >= w);
    CHECK(AlignedHeight(AlignedHeight(w)) == AlignedHeight(w));
    CHECK(AlignedHeight(w) % 8 == 0);
    CHECK(AlignedHeight(w) >= w);
  }

  // Source pitch / staging: the proven-good 4096 pitch, aligned extents.
  CHECK(SourcePitch(480 * 4) == 4096);   // 1920 -> 4096
  CHECK(SourcePitch(512 * 4) == 4096);   // 2048 -> 4096 (aligned 480)
  CHECK(SourcePitch(256 * 4) == 4096);   // 1024 -> 4096 (aligned 240, RGB10A2)
  CHECK(SourcePitch(960 * 4) == 4096);   // 3840 -> 4096
  CHECK(SourcePitch(4096) == 4096);
  CHECK(SourcePitch(4097) == 8192);
  CHECK(StagingBytes(4096, 360) == 1474560);  // run-076 upload #1 staging
  CHECK(StagingBytes(4096, 184) == 753664);   // aligned 240x184
  CHECK(PageRound(1474560) == 1474560);       // already page-multiple
  CHECK(PageRound(753664) == 753664);
  CHECK(PageRound(1) == 4096);
  // Staging always covers the aligned rect's bytes (the extent check the
  // probe logs): pitch >= aligned row bytes, rows >= aligned height.
  for (uint32_t w : {1u, 63u, 64u, 240u, 480u, 960u, 1280u}) {
    for (uint32_t h : {1u, 7u, 8u, 180u, 360u, 720u}) {
      const uint32_t aw = AlignedWidth(w), ah = AlignedHeight(h);
      const uint32_t pitch = SourcePitch(aw * 4);
      const uint32_t staging = PageRound(StagingBytes(pitch, ah));
      CHECK(pitch >= aw * 4);
      CHECK(staging >= uint64_t(aw) * ah * 4);
    }
  }

  // Prologue cap: 128 copies x 512 B budget fits 64 KB; 129 does not.
  CHECK(PrologueCapFits(128, 512, 64 << 10));
  CHECK(!PrologueCapFits(129, 512, 64 << 10));
  CHECK(PrologueCapFits(0, 512, 64 << 10));

  // Cohort release: needs a LATER frame's present fence, never equality/age.
  CHECK(!CohortReleased(0, 0));
  CHECK(CohortReleased(1, 0));
  CHECK(!CohortReleased(4, 4));
  CHECK(CohortReleased(5, 4));
  CHECK(CohortReleased(UINT64_MAX, UINT64_MAX - 1));

  // Bind rule: depth-skipped never; never-uploaded placeholder under delay;
  // one-frame delay boundary; legacy path binds immediately.
  CHECK(!BindAllowed(100, UINT64_MAX, true));
  CHECK(!BindAllowed(100, UINT64_MAX, false));
  CHECK(!BindAllowed(100, 0, true));
  CHECK(BindAllowed(100, 0, false));
  CHECK(!BindAllowed(3, 4, true));
  CHECK(BindAllowed(4, 4, true));
  CHECK(BindAllowed(5, 4, true));

  if (g_failures) {
    std::printf("upload_math_test: %d FAILURE(S)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("upload_math_test: ALL PASS (alignment sweep 1..2048, pitch/staging, "
              "prologue cap, cohort release, bind rule)\n");
  return EXIT_SUCCESS;
}
