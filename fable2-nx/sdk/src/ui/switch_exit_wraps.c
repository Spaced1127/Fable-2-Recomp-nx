/*
 * Run-072: hook EVERY deliberate process-exit path, not just exit()/abort().
 *
 * Run-071 proved the ~9.3 s death fires none of the atexit/abort/terminate
 * hooks: no rex_exit_marker.log entry, no new crash block, no GPU error state.
 * The paths that bypass atexit are _exit/_Exit (newlib), __libnx_exit (libnx,
 * used by the app's ExitProcess and OnClosing), svcExitProcess (the syscall
 * itself) and __assert_func (newlib's assert backend). The linker --wrap flags
 * in rexglue_switch.cmake route every reference - including the ones inside
 * libnx/newlib/deko3d - through these wrappers, which write the crash block
 * (build tag + caller stack + last-host-action ring + per-boot marker) BEFORE
 * calling the real function. Whatever ends the process now names itself.
 *
 * Compiled into the executable via the rexglue_switch_startup OBJECT library,
 * next to switch_crash_hooks.c.
 */

#include <stdio.h>
#include <switch.h>

/* exception_handler_switch.cpp (extern "C"). Writes the marker FIRST, then the
 * crash block with the frame chain, the stack scan and the host-action ring. */
extern void RexSwitchCrashLog(const char* reason, const ThreadExceptionDump* ctx, u64 stack,
                              u64 pc);

void __real_svcExitProcess(void);
void __real___libnx_exit(int rc);
void __real__exit(int rc);
void __real__Exit(int rc);
void __real_exit(int rc);
void __real___assert_func(const char* file, int line, const char* func, const char* failedexpr);

#define REX_EXIT_LOG(reason)                                                  \
  RexSwitchCrashLog((reason), NULL, (u64)__builtin_frame_address(0),          \
                    (u64)__builtin_return_address(0))

void __wrap_svcExitProcess(void) {
  REX_EXIT_LOG("svcExitProcess (direct syscall)");
  __real_svcExitProcess();
  __builtin_unreachable();
}

void __wrap___libnx_exit(int rc) {
  char reason[96];
  snprintf(reason, sizeof(reason), "__libnx_exit(%d)", rc);
  REX_EXIT_LOG(reason);
  __real___libnx_exit(rc);
  __builtin_unreachable();
}

void __wrap__exit(int rc) {
  char reason[96];
  snprintf(reason, sizeof(reason), "_exit(%d) - bypasses atexit", rc);
  REX_EXIT_LOG(reason);
  __real__exit(rc);
  __builtin_unreachable();
}

void __wrap__Exit(int rc) {
  char reason[96];
  snprintf(reason, sizeof(reason), "_Exit(%d) - bypasses atexit", rc);
  REX_EXIT_LOG(reason);
  __real__Exit(rc);
  __builtin_unreachable();
}

void __wrap_exit(int rc) {
  /* The atexit hook also logs; this one captures the CALLER's stack, which is
   * the site that decided to exit. Two blocks are fine - both are tagged. */
  char reason[96];
  snprintf(reason, sizeof(reason), "exit(%d) call site", rc);
  REX_EXIT_LOG(reason);
  __real_exit(rc);
  __builtin_unreachable();
}

void __wrap___assert_func(const char* file, int line, const char* func, const char* failedexpr) {
  char reason[640];
  snprintf(reason, sizeof(reason), "__assert_func: %s:%d %s: %s", file ? file : "?", line,
           func ? func : "?", failedexpr ? failedexpr : "?");
  REX_EXIT_LOG(reason);
  __real___assert_func(file, line, func, failedexpr);
  __builtin_unreachable();
}
