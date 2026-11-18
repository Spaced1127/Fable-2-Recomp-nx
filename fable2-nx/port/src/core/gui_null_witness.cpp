// gui_null_witness.cpp - one-shot witness for the M1 guest null-deref.
//
// The crash is `lwz r9, 36(r3)` in sub_82235698 (host 0x212f9e4) with
// r3 = 0, reached through sub_822358A8 -> sub_822357D0 -> sub_82235698.
// See docs/STATUS.md ("M1 crash analysis") for the established chain.
//
// This hook is passive: it reads only values the calling guest code read a
// moment earlier (so they are mapped), writes nothing to guest memory, and
// then runs the real function unchanged. When r3 is 0 - the crash case - it
// records the caller's return address and the singleton chain the six call
// sites use, so the field that was never populated can be named from a single
// hardware run.
//
// Remove this file from FABLE2_SOURCES once M1 is closed.

#include <atomic>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

REX_EXTERN(__imp__sub_822358A8);
REX_EXTERN(__imp__sub_8234EC08);
REX_EXTERN(__imp__sub_8245D088);
REX_EXTERN(__imp__sub_8233EEE0);
REX_EXTERN(__imp__sub_8233F2D8);
REX_EXTERN(__imp__sub_825F3050);
REX_EXTERN(__imp__sub_825F45A8);
REX_EXTERN(__imp__sub_825F4310);
REX_EXTERN(__imp__sub_82426ED8);
REX_EXTERN(__imp__sub_822659E8);

namespace {

// Global the six call sites load first (lis -31927; lwz 26912).
constexpr uint32_t kRoot = 0x83496920u;

// Reads a big-endian guest word. The caller has just dereferenced every
// pointer on this chain, so the pages are mapped; the range check only keeps
// a partially written chain from turning a diagnostic into a second fault.
uint32_t GuestU32(uint8_t* base, uint32_t addr) {
  if (addr == 0 || addr > 0xFFFFFFFCu) {
    return 0;
  }
  return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + addr));
}

void Witness(uint32_t n, const PPCContext& ctx, uint8_t* base) {
  const uint32_t a = GuestU32(base, kRoot);
  const uint32_t b = GuestU32(base, a + 12);
  const uint32_t c = GuestU32(base, b + 88);
  const uint32_t d = GuestU32(base, c + 4);
  const uint32_t e = GuestU32(base, d + 8);
  const uint32_t f = GuestU32(base, e + 4);
  const uint32_t g = GuestU32(base, f + 36);
  const uint32_t h = GuestU32(base, g + 4);
  const uint32_t i = GuestU32(base, h + 100);

  // warn, not info: the SDK flushes warnings to the card immediately, and the
  // process does not survive the call this witness is watching for.
  REXLOG_WARN("[gui-witness] #{} sub_822358A8 r3=0 lr={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X}",
              n, static_cast<uint32_t>(ctx.lr), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  REXLOG_WARN("[gui-witness]   {:08X} a={:08X} b={:08X} c={:08X} d={:08X} e={:08X} f={:08X} g={:08X} h={:08X} i={:08X}",
              kRoot, a, b, c, d, e, f, g, h, i);
  // Run 044: name the class. The vtable at [h] identifies the object; the first
  // words cover the page fields sub_82235698 uses (+8 table, +36/+40 origin) and
  // +0x64. The element at r4 is dumped the same way for correlation.
  const uint32_t vt_h = GuestU32(base, h);
  const uint32_t vt_r4 = GuestU32(base, ctx.r4.u32);
  REXLOG_WARN("[gui-witness]   class: h={:08X} vt={:08X} w8={:08X} w36={:08X} w40={:08X} w64={:08X}",
              h, vt_h, GuestU32(base, h + 8), GuestU32(base, h + 36), GuestU32(base, h + 40),
              GuestU32(base, h + 100));
  REXLOG_WARN("[gui-witness]   class: r4={:08X} vt={:08X} w0={:08X} w4={:08X} w8={:08X} wC={:08X}",
              ctx.r4.u32, vt_r4, GuestU32(base, ctx.r4.u32), GuestU32(base, ctx.r4.u32 + 4),
              GuestU32(base, ctx.r4.u32 + 8), GuestU32(base, ctx.r4.u32 + 12));
}

}  // namespace

#if defined(FABLE2_D3D_CENSUS)
// The census build already wraps this function (src/d3d_census_hooks.cpp);
// a second definition would fail to link.
#else
REX_HOOK_RAW(sub_822358A8) {
  if (ctx.r3.u32 == 0) {
    static std::atomic<uint32_t> seen{0};
    const uint32_t n = seen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4) {
      Witness(n, ctx, base);
    }
    // Run 052: the callee dereferences r3 unconditionally (lwz r9,36(r3) in
    // sub_82235698), so a null page set has no valid interpretation. Skipping
    // the call leaves the element's UV rects untouched instead of taking the
    // process down. This removes the crash, not the missing world entry - see
    // docs/STATUS.md (the producer is still open).
    REXLOG_WARN("[gui-guard] #{} skipped sub_822358A8 with r3=0 (lr={:08X})", n,
                static_cast<uint32_t>(ctx.lr));
    return;
  }
  __imp__sub_822358A8(ctx, base);
}
#endif

#if !defined(FABLE2_D3D_CENSUS)
// Run 045: the writer of CECPlayerControl::+0x64. sub_8234EC08 is called from
// the update loop (sub_825F3050, guest 0x825F3538); it releases the old object
// at this+0x64 and stores a new 56-byte object created from the rect at
// this+0x98..0xA4. If the write path is fine, +64 becomes non-zero; if it never
// runs on the faulting object, this is an ordering problem; if it stores 0, the
// 56-byte allocation/constructor failed.
REX_HOOK_RAW(sub_8234EC08) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t r0 = GuestU32(base, self + 0x98);
  const uint32_t r1 = GuestU32(base, self + 0x9C);
  __imp__sub_8234EC08(ctx, base);
  static std::atomic<uint32_t> calls{0};
  const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 80) {
    REXLOG_WARN("[gui-writer] #{} self={:08X} rect={:08X},{:08X} +64={:08X}", n, self, r0, r1,
                GuestU32(base, self + 0x64));
  }
}

#if !defined(FABLE2_D3D_CENSUS)
// Run 046: the HUD state machine (sub_8245D088). State is +44 (1..9); case 2
// gates the player-control update on byte +50. In run 045 the +0x64 writer never
// ran, so this gate or the state never reached case 2. Log the state and flag on
// change and the first 40 calls.
REX_HOOK_RAW(sub_8245D088) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t state = GuestU32(base, self + 44);
  const uint32_t flag = (GuestU32(base, self + 48) >> 8) & 0xFF;  // byte +50 (big-endian)
  static std::atomic<uint32_t> calls{0};
  static uint32_t last = 0xFFFFFFFFu;
  const uint32_t n = calls.fetch_add(1) + 1;
  const uint32_t cur = (state << 8) | (flag & 0xFF);
  if (n <= 40 || cur != last) {
    REXLOG_WARN("[gui-state] #{} self={:08X} state={} flag={} ", n, self, state, flag & 0xFF);
    last = cur;
  }
  __imp__sub_8245D088(ctx, base);
}
#endif

#if !defined(FABLE2_D3D_CENSUS)
// Run 047: the update call and the player lookup. The state machine reaches
// case 2 with flag=1, yet the +0x64 writer never runs; these log which object
// the update targets and what the lookup returns.
REX_HOOK_RAW(sub_8233EEE0) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32;
  __imp__sub_8233EEE0(ctx, base);
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 40) {
    const uint32_t n1 = GuestU32(base, self + 0x24);
    const uint32_t h2 = GuestU32(base, n1 + 4);
    REXLOG_WARN("[gui-upd] #{} self={:08X} r4={:08X} r5={:08X} r6={:08X} w36={:08X} idB4={:08X} +64={:08X} n24={:08X} h2={:08X} h2+64={:08X}",
                n, self, a4, a5, a6, GuestU32(base, self + 36), GuestU32(base, self + 0xB4),
                GuestU32(base, self + 0x64), n1, h2, GuestU32(base, h2 + 0x64));
  }
}
REX_HOOK_RAW(sub_8233F2D8) {
  const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;
  __imp__sub_8233F2D8(ctx, base);
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 40) {
    REXLOG_WARN("[gui-lookup] #{} in3={:08X} in4={:08X} in5={:08X} -> {:08X}", n, a3, a4, a5,
                ctx.r3.u32);
  }
}
#endif

#if !defined(FABLE2_D3D_CENSUS)
// Run 048: does the update pass itself run for the instance sub_8233EEE0 got?
REX_HOOK_RAW(sub_825F3050) {
  const uint32_t self = ctx.r3.u32;
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 20) REXLOG_WARN("[gui-pass] #{} self={:08X}", n, self);
  __imp__sub_825F3050(ctx, base);
}

// Run 049: mid-asm markers inside sub_8233EEE0 (manifest entries at
// 0x8233EFD4 and 0x8233F010). See docs/RUN_049.md. r31 is the component
// (self), r29 is 0x834C0000 so [r29-6572] = 0x834BE654 is the wait-loop byte,
// and r3 is whatever the last call returned - logged as-is, never trusted as
// a pointer (only read when it lands in the guest virtual heap).
namespace {

uint8_t* GuestBase() {
  return rex::system::kernel_state()->memory()->virtual_membase();
}

// Reads a word only from the guest virtual heap (0x40000000..0x5FFFFFFF,
// where every CECPlayerControl instance seen so far lives); anything else is
// reported as 0 instead of risking a read of an uncommitted page.
uint32_t GuestHeapU32(uint8_t* base, uint32_t addr) {
  if (addr < 0x40000000u || addr >= 0x60000000u) {
    return 0;
  }
  return GuestU32(base, addr);
}

void GuiMark(const char* where, uint32_t n, uint32_t r3, uint32_t r29, uint32_t r31) {
  uint8_t* base = GuestBase();
  const uint32_t mgr = GuestHeapU32(base, r31 + 4);
  const uint32_t n24 = GuestHeapU32(base, r31 + 0x24);
  const uint32_t h2 = GuestHeapU32(base, n24 + 4);
  const uint32_t loop_addr = r29 >= 6572 ? r29 - 6572 : 0x834BE654u;
  const uint32_t loop_byte = GuestU32(base, loop_addr) >> 24;  // lbz at that address
  REXLOG_WARN(
      "[gui-mark] {} #{} self={:08X} w36={:08X} mgr={:08X} n24={:08X} h2={:08X} "
      "loop={:08X} r3={:08X} w3_36={:08X}",
      where, n, r31, GuestHeapU32(base, r31 + 36), mgr, n24, h2, loop_byte, r3,
      GuestHeapU32(base, r3 + 36));
}

}  // namespace

void fable2_hook_gui_mark_wait(PPCRegister& r3, PPCRegister& r29, PPCRegister& r31) {
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 80) GuiMark("wait", n, r3.u32, r29.u32, r31.u32);
}

void fable2_hook_gui_mark_pass(PPCRegister& r3, PPCRegister& r29, PPCRegister& r31) {
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 80) GuiMark("pass", n, r3.u32, r29.u32, r31.u32);
}

// --- Run 050: why is the UI batch empty? -----------------------------------
//
// Run 049 confirmed the update reaches the pass: [gui-mark] pass at 22.413 with
// self=42220210, h2=42538C10, then [gui-pass] 4 ms later, then a 4.1 s gap
// before [gui-upd]. Static reading of sub_825F3050 shows it calls
// sub_825F45A8 and jumps straight to its cleanup path (loc_825F389C) when that
// returns 0, skipping the whole body - including the writer that fills +0x64.
//
// These hooks add no behaviour: they log the value that decides the branch
// (sub_825F45A8's return), the batch vectors it fills on the caller's stack
// (twelve {begin,end,cap} triples at sp+100, sp+116 ... sp+276), the bit test
// inside sub_825F45A8 that produces a 0 return, and - if the theory is wrong -
// the writer gate itself. All reads are range-checked.

namespace {

constexpr uint32_t kBatchOff[12] = {100, 116, 132, 148, 164, 180,
                                    196, 212, 228, 244, 260, 276};

// Guest regions this build reads: the refcounted heap, the guest stacks, and
// the loaded image/.data (the crash-chain globals at 0x8349xxxx live there).
inline bool ReadableGuest(uint32_t addr) {
  return (addr >= 0x40000000u && addr < 0x60000000u) ||
         (addr >= 0x60000000u && addr < 0x80000000u) ||
         (addr >= 0x82000000u && addr < 0x84000000u);
}

uint32_t SafeU32(uint8_t* base, uint32_t addr) {
  if (!ReadableGuest(addr)) return 0;
  return GuestU32(base, addr);
}

// Copies up to n bytes as printable ASCII. Used for the two static key strings
// and for whatever the stack object points at; a wrong guess only costs dots.
void AsciiDump(char* out, int cap, uint8_t* base, uint32_t addr, int n) {
  int j = 0;
  for (int i = 0; i < n && j < cap - 1; ++i) {
    const uint32_t a = addr + static_cast<uint32_t>(i);
    if (!ReadableGuest(a)) break;
    const uint8_t c = *reinterpret_cast<volatile uint8_t*>(base + a);
    out[j++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
  }
  out[j] = 0;
}

// Twelve vector counts from the batch object at the pass's stack. A begin/end
// pair that is not ordered is reported as FFFFFFFF rather than as a huge count.
void BatchLine(const char* tag, uint32_t n, uint32_t sp, uint32_t ret) {
  uint8_t* base = GuestBase();
  uint32_t c[12];
  for (int i = 0; i < 12; ++i) {
    const uint32_t b = SafeU32(base, sp + kBatchOff[i]);
    const uint32_t e = SafeU32(base, sp + kBatchOff[i] + 4);
    c[i] = (e >= b) ? static_cast<uint32_t>((e - b) >> 2) : 0xFFFFFFFFu;
  }
  REXLOG_WARN("[gui-batch] {} #{} sp={:08X} ret={:08X} c=[{},{},{},{} | {},{},{},{} | {},{},{},{}]",
              tag, n, sp, ret, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8],
              c[9], c[10], c[11]);
}

}  // namespace

// The batch builder. Returns 1 when the batch is usable, 0 when it is not; the
// caller's branch on that byte is the whole gate.
REX_HOOK_RAW(sub_825F45A8) {
  const uint32_t mgr = ctx.r3.u32;
  const uint32_t a4 = ctx.r4.u32;
  const uint32_t a5 = ctx.r5.u32;
  const uint32_t host = ctx.r6.u32;
  const uint32_t out = ctx.r7.u32;
  __imp__sub_825F45A8(ctx, base);
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 24) {
    REXLOG_WARN("[gui-batch] fn #{} mgr={:08X} a4={:08X} key={:08X} host={:08X} out={:08X} -> {:08X}",
                n, mgr, a4, a5, host, out, ctx.r3.u32);
    BatchLine("fn", n, out - 96, ctx.r3.u32);
  }
}

// 0x825F3208: first instruction after `bl sub_825F45A8`. r3 is the deciding
// byte; r1 is the pass frame the batch was written into.
void fable2_hook_gui_batch_branch(PPCRegister& r3, PPCRegister& r1) {
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 24) BatchLine("br", n, r1.u32, r3.u32);
}

// 0x825F389C: the early-exit target. r19/r18 are the last vector's begin/end
// (loaded just above the branch); r14/r16/r17 are three more vector bases.
void fable2_hook_gui_batch_exit(PPCRegister& r1, PPCRegister& r14, PPCRegister& r16,
                                PPCRegister& r17, PPCRegister& r18, PPCRegister& r19) {
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 24) {
    REXLOG_WARN("[gui-batch] exit #{} sp={:08X} r14={:08X} r16={:08X} r17={:08X} r18={:08X} r19={:08X} n12={}",
                n, r1.u32, r14.u32, r16.u32, r17.u32, r18.u32, r19.u32,
                r18.u32 >= r19.u32 ? (r18.u32 - r19.u32) >> 2 : 0xFFFFFFFFu);
  }
}

// 0x825F4668: right after the sub_82C63040 bit test inside sub_825F45A8. This
// is the only place that returns 0. Logs the bit plus the key strings: the two
// static objects 0x8349750C / 0x83497510 and whatever the stack object holds.
void fable2_hook_gui_bit(PPCRegister& r3, PPCRegister& r1) {
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 24) {
    uint8_t* base = GuestBase();
    const uint32_t sp = r1.u32;
    const uint32_t k0 = SafeU32(base, 0x8349750Cu);
    const uint32_t k1 = SafeU32(base, 0x83497510u);
    const uint32_t so = SafeU32(base, sp + 92);
    const uint32_t sw = SafeU32(base, sp + 96);
    char s0[36], s1[36], s2[36];
    AsciiDump(s0, 36, base, k0, 32);
    AsciiDump(s1, 36, base, k1, 32);
    AsciiDump(s2, 36, base, so, 32);
    REXLOG_WARN("[gui-batch] bit #{} ret={:08X} k0={:08X} '{}' k1={:08X} '{}' so={:08X} sw={:08X} '{}' obj={:08X},{:08X},{:08X},{:08X}",
                n, r3.u32, k0, s0, k1, s1, so, sw, s2,
                SafeU32(base, sp + 128), SafeU32(base, sp + 132),
                SafeU32(base, sp + 136), SafeU32(base, sp + 140));
  }
}

// 0x825F3510: the writer gate, only reached if the batch was usable. Logs the
// hash and the wrapper/control chain so a wrong theory is visible immediately.
void fable2_hook_gui_gate(PPCRegister& r3, PPCRegister& r26) {
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 24) {
    uint8_t* base = GuestBase();
    const uint32_t host = SafeU32(base, r26.u32 + 4);
    const uint32_t wrap = SafeU32(base, host + 36);
    const uint32_t ctl = SafeU32(base, wrap + 4);
    REXLOG_WARN("[gui-batch] gate #{} hash={:08X} r26={:08X} host={:08X} wrap={:08X} ctl={:08X} c180={:08X}",
                n, r3.u32, r26.u32, host, wrap, ctl, SafeU32(base, ctl + 180));
  }
}

// --- Run 053: guard the element, not one callee ---------------------------
//
// Run 052's guard on sub_822358A8 moved the fault 3 ms later to the same null
// page set, consumed by the element's rect/blit routine sub_82426ED8:
//
//   r5  = [[r3+4]+4]            ; the element host
//   r10 = [r5+36]               ; the ref-counted wrapper
//   r9  = [r10+4]               ; the CECPlayerControl
//   r21 = [r9+100]              ; the page set (control +0x64)  <- null
//   ... bl sub_822659E8(out, r3, r21, r4) -> r4 = r21 + 36 = 0x24 -> fault
//
// The page set is the whole element's texture source: with it null there is
// nothing valid to draw, so the element is skipped as a unit. Reads only the
// chain the callee itself dereferences on entry.
REX_HOOK_RAW(sub_82426ED8) {
  const uint32_t s1 = SafeU32(base, ctx.r3.u32 + 4);
  const uint32_t s2 = SafeU32(base, s1 + 4);
  const uint32_t s3 = SafeU32(base, s2 + 36);
  const uint32_t s4 = SafeU32(base, s3 + 4);
  const uint32_t page = SafeU32(base, s4 + 100);
  if (page == 0) {
    static std::atomic<uint32_t> c{0};
    const uint32_t n = c.fetch_add(1) + 1;
    if (n <= 8 || n == 100 || n == 1000) {
      REXLOG_WARN("[gui-guard] element #{} skipped: page set null (self={:08X} control={:08X})",
                  n, ctx.r3.u32, s4);
    }
    ctx.r3.u64 = 0;  // "nothing drawn"
    return;
  }
  __imp__sub_82426ED8(ctx, base);
}

// --- Run 054: the common clip choke point ---------------------------------
//
// Runs 052/053 each guarded one consumer of the null page set; the fault moved
// 3 ms later both times. The third site is again sub_82265A80 reached from
// sub_822659E8, which has ~75 callers - guarding callers cannot converge.
// sub_822659E8 itself does `addi r4, r5, 36` for the clip rect, so r5 == 0
// dereferences guest 0x24 no matter who calls. That is the choke point every
// element draw goes through: with no clip rect there is nothing to clip, so the
// output rect is zeroed (an empty rect draws nothing) and the call is skipped.
REX_HOOK_RAW(sub_822659E8) {
  if (ctx.r5.u32 == 0) {
    const uint32_t out = ctx.r3.u32;
    if (ReadableGuest(out)) {
      for (uint32_t i = 0; i < 16; i += 4) {
        *reinterpret_cast<volatile uint32_t*>(base + out + i) = 0;
      }
    }
    static std::atomic<uint32_t> c{0};
    const uint32_t n = c.fetch_add(1) + 1;
    if (n <= 8 || n == 100 || n == 1000) {
      REXLOG_WARN("[gui-guard] clip #{} skipped: clip rect null (out={:08X})", n, out);
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_822659E8(ctx, base);
}

// --- Run 052: localise the 4 s inside the pass setup ----------------------
//
// Run 051 showed the key builder takes ~2 ms, so the 4.0 s between the pass
// entry and the batch test is spent in the four calls sub_825F3050 makes before
// it: sub_825F2E10, sub_82369EE0, sub_8236A460, sub_82A492B0. One marker after
// each brackets them by timestamp. Passive: reads one register.
namespace {
void PhaseLine(const char* tag, uint32_t r3) {
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 24) REXLOG_WARN("[gui-phase] {} #{} r3={:08X}", tag, n, r3);
}
}  // namespace

void fable2_hook_gui_phase_a(PPCRegister& r3) { PhaseLine("after 825F2E10", r3.u32); }
void fable2_hook_gui_phase_b(PPCRegister& r3) { PhaseLine("after 82369EE0", r3.u32); }
void fable2_hook_gui_phase_c(PPCRegister& r3) { PhaseLine("after 8236A460", r3.u32); }
void fable2_hook_gui_phase_d(PPCRegister& r3) { PhaseLine("after 82A492B0", r3.u32); }

// --- Run 051: the composed key and the key-builder timing -------------------
//
// Run 050 named the lookup ("Data\Worlds\" template, not found) but the dump
// started at the string object, so it showed the 16-byte header and only the
// first 16 characters. sub_825F4310 is the builder and has exactly two call
// sites, both inside sub_825F45A8 (key 1 and key 2), so logging it entry/exit
// both recovers the full composed key and splits the 4.0 s stall: if the
// builder is fast, the time is in the container lookups after it.
REX_HOOK_RAW(sub_825F4310) {
  const uint32_t out = ctx.r3.u32;   // 4-byte slot the key is written to
  const uint32_t mgr = ctx.r4.u32;
  const uint32_t tmpl = ctx.r5.u32;  // static 4-byte slot (0x8349750C/510)
  const uint32_t a6 = ctx.r6.u32;
  const uint32_t a7 = ctx.r7.u32;
  const uint32_t a8 = ctx.r8.u32;
  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  const bool log = n <= 8;
  if (log) {
    uint8_t* base = GuestBase();
    const uint32_t tobj = SafeU32(base, tmpl);
    char t[56];
    AsciiDump(t, 56, base, tobj + 16, 40);
    REXLOG_WARN("[gui-key] enter #{} out={:08X} mgr={:08X} tmpl={:08X} ptr={:08X} '{}' a6={:08X} a7={:08X} a8={:08X}",
                n, out, mgr, tmpl, tobj, t, a6, a7, a8);
  }
  __imp__sub_825F4310(ctx, base);
  if (log) {
    uint8_t* base = GuestBase();
    const uint32_t sobj = SafeU32(base, out);
    char s16[80], s0[56];
    AsciiDump(s16, 80, base, sobj + 16, 72);
    AsciiDump(s0, 56, base, sobj, 40);
    REXLOG_WARN("[gui-key] exit  #{} out={:08X} obj={:08X} chars='{}' raw='{}'",
                n, out, sobj, s16, s0);
  }
}
#endif
#endif
