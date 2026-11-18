# Credits and third-party notices

## Project licence

This port is distributed under the **GNU General Public License, version 3**
(see `LICENSE`). Release notes must say so, and because the port contains
GPL-3.0 code, every release must offer the corresponding source; publishing this
repository satisfies that.

## Reference projects

### NaGaa95/UnleashedRecomp-NX

- Repository: https://github.com/NaGaa95/UnleashedRecomp-NX
- Commit consulted: `84932ed066fecf2670960f4296e2dc8404e2b975` (2026-07-30, "0.0.3")
- Licence: GNU GPL v3 (same licence as this port)

Consulted for technique, and the source of any file listed under "Copied files"
below. Nothing is vendored from its tree beyond the files named here.

### hedge-dev/UnleashedRecomp

Upstream project the fork above is derived from; credited for the platform-layer
design the fork's Switch work builds on.

## Copied files

None yet. The port's cross-thread synchronisation was compared against
`UnleashedRecomp/os/switch_atomic_wait.h` and did not need it: the submit epoch is
a non-blocking counter, the staging hand-off is ordered with release/acquire
fences, and swap-image ownership is deko3d's own. When a blocking wait is actually
needed (audio pump, frame pacing), that header's approach — Horizon's address
arbiter (`svcWaitForAddress` / `svcSignalToAddress`) with a 1 ms safety timeout —
is the pattern to copy, and doing so adds a file entry here with its copyright
header intact plus a line saying which file it came from.

Parked for the M2 input milestone: `UnleashedRecomp/hid/driver/switch_hid.cpp`
(libnx HID → pad mapping; not XInput, so the semantics need mapping before it is
usable here).