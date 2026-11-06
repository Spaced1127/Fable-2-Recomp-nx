// Fable II D3D-library call census (see tools/deko3d/gen_d3d_census_hooks.py).
//
// Every sub_ function in the D3D candidate clusters gets a counting hook. This
// module counts calls, samples the first caller (ctx.lr at entry) and the
// argument registers r3-r7, and every 5 s writes the functions that were
// called, sorted by call count, to the normal log. The call-rate and argument
// shapes identify the Direct3D API functions Renut needs (draws, KickOff,
// fences, Swap, Clear/Resolve, shader registration, ...).
//
// Diagnostic only: built with -DFABLE2_D3D_CENSUS=ON, absent otherwise.

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace fable2::census {

// Called from every generated hook before the original function.
void OnCall(uint32_t index, const PPCContext& ctx, uint8_t* base);

}  // namespace fable2::census
