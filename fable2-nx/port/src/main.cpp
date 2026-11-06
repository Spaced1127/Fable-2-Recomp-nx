// fable_2 - ReXGlue Recompiled Project (Nintendo Switch)

#include "generated/default/fable_2_init.h"

#include "fable2_app.h"

#include <rex/cvar.h>

// Skip the default.xex SHA-256 integrity check at startup (see
// Fable2App::OnLoadXexImage). When set, the XEX hash is still computed and
// written to the log, but a mismatch (or an unreadable file) no longer blocks
// the launch. Useful for running a modified or non-catalogue default.xex -
// e.g. a base-game rev whose hash is not in the verified catalogue.
REXCVAR_DEFINE_BOOL(skipHash, false, "Launch",
                    "Skip the default.xex SHA-256 integrity check. The hash is "
                    "still logged, but a mismatch no longer blocks the launch "
                    "(useful for a modified or non-catalogue XEX). On Switch "
                    "set it as a flat key in fable_2.toml (the SDK's config "
                    "loader prefixes section names, so no [table] wrapper).");

REX_DEFINE_APP(fable_2, Fable2App::Create)
