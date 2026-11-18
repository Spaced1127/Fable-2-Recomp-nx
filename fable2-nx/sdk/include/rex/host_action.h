/**
 * @file        rex/host_action.h
 * @brief       Last-host-action ring for the silent-death investigation (run-071).
 *
 * The ~9.5 s death writes no crash block: runs 059-070 appended NOTHING to
 * rex_crash.log, whose exit()/abort() blocks date to runs 057/058 (the file hash
 * has been frozen since 058). So the current death bypasses every hook - an
 * OS-level kill (GPU channel fault) or a direct svcExitProcess. This ring names
 * the last host actions either way:
 *
 *  - NoteHostAction: ring + a per-line-flushed file (logs/rex/rex_actions.log).
 *    Use for low-rate events: uploads, skips, IO failures, XAM/HAL calls, errors.
 *  - NoteHostActionFast: ring only (no SD write). Use for per-frame events.
 *  - The crash logger (exit/abort/terminate/data-abort) dumps the ring into
 *    rex_crash.log, so even a silent-ish death names its last actions.
 *
 * Lock-free by design: a faulting thread may be inside NoteHostAction when the
 * crash logger runs; the dump must not wait on it.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex {

// printf-style; the text is truncated to 159 chars. Thread-safe, no allocation.
void NoteHostAction(const char* fmt, ...);
// Ring only - for per-frame events (no SD round trip).
void NoteHostActionFast(const char* fmt, ...);

// Chronological dump of the ring into out (NUL-terminated). Returns bytes
// written. Safe to call from the crash logger (no locks).
size_t DumpHostActions(char* out, size_t cap);
// Same, but only the last `count` entries (for periodic log lines).
size_t DumpHostActionsLast(char* out, size_t cap, size_t count);

// Build identity for the crash blocks and the marker file (set once at startup).
void SetHostActionBuildTag(const char* tag);

}  // namespace rex

extern "C" {
// C bridge for switch_crash_hooks.c.
size_t RexDumpHostActions(char* out, size_t cap);
void RexSetCrashBuildTag(const char* tag);
}
