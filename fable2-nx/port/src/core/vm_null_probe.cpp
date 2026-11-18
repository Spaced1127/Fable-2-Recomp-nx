// vm_null_probe.cpp - run-056: name the missing VM argument behind the
// `read of guest 0x00000000` crash.
//
// What run 054/055 already proved, and this build starts from:
//
//   crash record  pc imagen+0x96b688  ->  sub_821C95D8 + 0xD8
//   addr2line     port/generated/default/fable_2_recomp.21.cpp:370
//   guest code    lwz r3, 0(r3)        (immediately after `bl 0x822281F8`)
//   host code     ldr w0, [x20, x0]    with x0 = 0, x20 = the guest base
//
// so the guest itself loaded guest address 0: the value the argument fetch
// returned was NULL, and the handler dereferenced it without a check.
//
// sub_822281F8 (recompiled as the deduplicated body
// ProcessAndProcessAndProcess206_822281F8) is a three-way wrapper around
// AccessElement8_82A1E620, the VM argument accessor:
//
//   element = AccessElement8_82A1E620(vm, index);   // {value @+0, type @+4}
//   type = element->type;
//   if (type == 2) return element->value;
//   if (type == 7) return element->value + 24;
//   return 0;                                       // <- the NULL
//
// AccessElement8_82A1E620 is a real symbol (strong __imp__ body, weak alias),
// so the fetch can be watched without regenerating the guest code: the wrapper
// calls it, and its guest return address at the accessor's entry is 0x82228208.
// Every accessor call made from that address whose tag is neither 2 nor 7 is
// exactly the returning-NULL case. The hook names it: vm, argument index,
// element, tag, value word, the neighbouring argument slots, and the string the
// value points at. The sub_821C95D8 entry hook adds the dispatch caller and
// keeps the last few dispatches, because the crash line has to be read with the
// call that produced it.
//
// Diagnostic only: nothing is guarded or skipped here, so the run still ends at
// the fault with the evidence in the log. Remove this file from FABLE2_SOURCES
// once the value is fixed at its producer.

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

REX_EXTERN(__imp__sub_821C95D8);
REX_EXTERN(__imp__AccessElement8_82A1E620);
REX_EXTERN(__imp__GuildChest_ProcessItem_82228830);
REX_EXTERN(__imp__ProcessAndConstructAndAtomicIncrement_82C64018);

namespace {

// Return address inside sub_822281F8, i.e. the instruction after the
// `bl AccessElement8_82A1E620` the wrapper makes for every argument fetch.
constexpr uint32_t kFetchCaller = 0x82228208u;

// Guest regions this build reads. Everything else is reported as 0 rather than
// risking a second fault inside the probe.
inline bool Readable(uint32_t addr) {
  return (addr >= 0x40000000u && addr < 0x60000000u) ||
         (addr >= 0x60000000u && addr < 0x80000000u) ||
         (addr >= 0x82000000u && addr < 0x84000000u);
}

uint32_t U32(uint8_t* base, uint32_t addr) {
  if (!Readable(addr)) return 0;
  return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + addr));
}

void Ascii(char* out, int cap, uint8_t* base, uint32_t addr, int n) {
  int j = 0;
  for (int i = 0; i < n && j < cap - 1; ++i) {
    const uint32_t a = addr + static_cast<uint32_t>(i);
    if (!Readable(a)) break;
    const uint8_t c = *reinterpret_cast<volatile uint8_t*>(base + a);
    out[j++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
  }
  out[j] = 0;
}

// Run-058: name a VM object without assuming its layout.
//
// Run 057 printed `[vm-name] ... 'PP.4......'` / `'CI32Vector2'`, i.e. the text
// was NOT at the offset the GUI string class uses (+16). Rather than guess once
// more, every plausible slot is printed: the object's first six words, the text
// at +0/+4/+8/+0xC/+0x10, and one indirection through each of the first three
// words (a string object usually holds a pointer to its characters somewhere in
// that prefix). Whichever line contains real text names the thing.
void NameCandidates(const char* tag, uint32_t n, uint8_t* base, uint32_t value) {
  auto text_at = [&](uint32_t addr, char* out, int cap) { Ascii(out, cap, base, addr, 28); };
  char w[200];
  int used = 0;
  used += std::snprintf(w + used, sizeof(w) - size_t(used),
                        "words %08X %08X %08X %08X %08X %08X", U32(base, value + 0),
                        U32(base, value + 4), U32(base, value + 8), U32(base, value + 0xC),
                        U32(base, value + 0x10), U32(base, value + 0x14));
  char t0[32], t4[32], t8[32], tC[32], t10[32];
  text_at(value + 0, t0, 28);
  text_at(value + 4, t4, 28);
  text_at(value + 8, t8, 28);
  text_at(value + 0xC, tC, 28);
  text_at(value + 0x10, t10, 28);
  REXLOG_WARN("[vm-name] {} #{} obj={:08X} {}", tag, n, value, w);
  REXLOG_WARN("[vm-name]   inline +0 '{}' +4 '{}' +8 '{}' +C '{}' +10 '{}'", t0, t4, t8, tC,
              t10);
  // One level of indirection: a string/type object points at its characters.
  for (uint32_t off = 0; off <= 8; off += 4) {
    const uint32_t p = U32(base, value + off);
    if (!Readable(p)) continue;
    char a[32], b[32];
    text_at(p, a, 28);
    text_at(p + 0x10, b, 28);
    REXLOG_WARN("[vm-name]   via [+{:X}]={:08X} '{}' | +10 '{}'", off, p, a, b);
  }
}

uint32_t ThreadId() {
  auto* ts = rex::runtime::ThreadState::Get();
  return ts ? ts->thread_id() : 0;
}

// Last argument fetches made through sub_822281F8, newest at the highest
// sequence number. Two guest threads reach this path, so the ring is a window,
// not a history: at the fault the newest entry is the failing fetch.
struct Fetch {
  uint32_t vm;
  uint32_t index;
  uint32_t element;
  uint32_t type;
  uint32_t value;
};
constexpr uint32_t kFetchRing = 8;
Fetch g_fetches[kFetchRing]{};
std::atomic<uint32_t> g_fetch_seq{0};

// Last script dispatches into sub_821C95D8 (the opcode handler that faults).
struct Dispatch {
  uint32_t caller;
  uint32_t vm;
  uint32_t r4;
  uint32_t r5;
  uint32_t r6;
  uint32_t r7;
};
constexpr uint32_t kDispatchRing = 8;
Dispatch g_dispatches[kDispatchRing]{};
std::atomic<uint32_t> g_dispatch_seq{0};

// Run-057: every VM invocation passes through 0x82228830 (the invoke helper:
// it reads the callee prototype at [function+16], fills missing parameters with
// type 0 = nil, then dispatches through the value's vtable). Its second
// argument is the callee *value* ({value @+0, type @+4}), so this ring says
// which function was being invoked, by name when the callee is a string
// (type 6) - the VM resolves `'...MarkerExit_Gate'.`-style names that way.
struct Invoke {
  uint32_t value;
  uint32_t type;
  uint32_t vm;
  uint32_t top;
};
constexpr uint32_t kInvokeRing = 10;
Invoke g_invokes[kInvokeRing]{};
std::atomic<uint32_t> g_invoke_seq{0};

// Run-058: only the *named* callees (type 6), in their own ring, so the names
// around the fault are never crowded out by the hundreds of ordinary calls.
constexpr uint32_t kNameRing = 16;
uint32_t g_names[kNameRing]{};
std::atomic<uint32_t> g_name_seq{0};

// Run-059: the engine's name-table lookup (guest 0x82C64018) with the caller LR,
// the requested key's text and the found object. This is the function the GUI
// batch builder uses to resolve "Data\Worlds\Albion\", so a second caller LR
// with an entity-looking key says whether entities share the same table - and a
// null result names the miss directly.
struct Lookup {
  uint32_t caller;
  uint32_t key_obj;
  uint32_t found0;
  uint32_t found8;
};
constexpr uint32_t kLookupRing = 16;
Lookup g_lookups[kLookupRing]{};
std::atomic<uint32_t> g_lookup_seq{0};

// One 8-byte VM slot: {value, type}. `index` names the argument slot by the
// accessor's own rule so the log can be read without recomputing it.
void SlotLine(const char* tag, uint8_t* base, uint32_t element, int32_t index) {
  REXLOG_WARN("[vm-null]   {} idx={} elem={:08X} value={:08X} type={:08X} ({})", tag, index,
              element, U32(base, element + 0), U32(base, element + 4), U32(base, element + 4));
}

void DumpFailure(uint32_t n, uint8_t* base, uint32_t vm, uint32_t index, uint32_t element,
                 uint32_t type, uint32_t value) {
  REXLOG_WARN("[vm-null] #{} tid={} vm={:08X} index={} elem={:08X} type={:08X} value={:08X}",
              n, ThreadId(), vm, static_cast<int32_t>(index), element, type, value);

  // The argument list around the failing slot: for index > 0 the elements are
  // contiguous ({value,type} pairs above [vm+12]), so +/- two slots shows the
  // whole call. Negative indices live below the frame base; they are still
  // valid slots and are printed when the read is in range.
  for (int32_t d = -2; d <= 2; ++d) {
    const uint32_t e = element + static_cast<uint32_t>(d * 8);
    if (!Readable(e) || !Readable(e + 4)) continue;
    SlotLine(d == 0 ? "slot*" : "slot ", base, e, static_cast<int32_t>(index) + d);
  }

  // The VM's own state: [vm+8] and [vm+12] are the frame base and the top of
  // the argument area the accessor indexes from.
  REXLOG_WARN("[vm-null]   vm words: +0={:08X} +4={:08X} +8={:08X} +C={:08X} +10={:08X} "
              "+14={:08X} +18={:08X} +1C={:08X} +20={:08X} +24={:08X} +28={:08X} +2C={:08X}",
              U32(base, vm + 0), U32(base, vm + 4), U32(base, vm + 8), U32(base, vm + 0xC),
              U32(base, vm + 0x10), U32(base, vm + 0x14), U32(base, vm + 0x18),
              U32(base, vm + 0x1C), U32(base, vm + 0x20), U32(base, vm + 0x24),
              U32(base, vm + 0x28), U32(base, vm + 0x2C));

  // Name the value. A script string is a heap object whose characters the
  // gui-key probe already found at +16; the raw dump catches a value that
  // points straight at characters instead.
  char s0[64], s16[64];
  Ascii(s0, 64, base, value, 48);
  Ascii(s16, 64, base, value + 16, 48);
  REXLOG_WARN("[vm-null]   value '{}' | +16 '{}'", s0, s16);

  // Every argument slot that carries a string, so the call's other operands are
  // named even when the failing one is nil.
  int named_args = 0;
  for (int32_t d = -2; d <= 3 && named_args < 2; ++d) {
    const uint32_t e = element + static_cast<uint32_t>(d * 8);
    if (!Readable(e) || !Readable(e + 4)) continue;
    if (U32(base, e + 4) != 6) continue;
    ++named_args;
    NameCandidates("arg", uint32_t(static_cast<int32_t>(index) + d), base, U32(base, e + 0));
  }

  // The newest fetches, so a slot that was never written can be told from a
  // slot written with the wrong type.
  const uint32_t seq = g_fetch_seq.load();
  for (uint32_t i = 0; i < kFetchRing; ++i) {
    if (seq < kFetchRing && i >= seq) break;
    const uint32_t k = seq >= kFetchRing ? seq - kFetchRing + i : i;
    const Fetch& f = g_fetches[k % kFetchRing];
    REXLOG_WARN("[vm-null]   fetch #{} vm={:08X} idx={} elem={:08X} type={:08X} value={:08X}",
                k + 1, f.vm, static_cast<int32_t>(f.index), f.element, f.type, f.value);
  }

  const uint32_t dseq = g_dispatch_seq.load();
  for (uint32_t i = 0; i < kDispatchRing; ++i) {
    if (dseq < kDispatchRing && i >= dseq) break;
    const uint32_t k = dseq >= kDispatchRing ? dseq - kDispatchRing + i : i;
    const Dispatch& d = g_dispatches[k % kDispatchRing];
    REXLOG_WARN("[vm-null]   dispatch #{} caller={:08X} vm={:08X} r4={:08X} r5={:08X} "
                "r6={:08X} r7={:08X}",
                k + 1, d.caller, d.vm, d.r4, d.r5, d.r6, d.r7);
  }

  // The invocations that led here, newest last. A type-6 callee is a name
  // lookup, so its characters name the script function ('...arkerExit_Gate'.);
  // a type-7 callee is an inline function object, dumped at its payload.
  const uint32_t iseq = g_invoke_seq.load();
  for (uint32_t i = 0; i < kInvokeRing; ++i) {
    if (iseq < kInvokeRing && i >= iseq) break;
    const uint32_t k = iseq >= kInvokeRing ? iseq - kInvokeRing + i : i;
    const Invoke& inv = g_invokes[k % kInvokeRing];
    char n0[56], n1[56];
    Ascii(n0, 56, base, inv.value, 40);
    Ascii(n1, 56, base, inv.value + 16, 40);
    REXLOG_WARN("[vm-null]   invoke #{} vm={:08X} callee={:08X} type={} top={:08X} "
                "'{}' | +16 '{}'",
                k + 1, inv.vm, inv.value, inv.type, inv.top, n0, n1);
  }

  // Run-058: the named callees, full candidate matrix. The last entries are the
  // ones the faulting call followed.
  const uint32_t nseq = g_name_seq.load();
  for (uint32_t i = 0; i < kNameRing; ++i) {
    if (nseq < kNameRing && i >= nseq) break;
    const uint32_t k = nseq >= kNameRing ? nseq - kNameRing + i : i;
    NameCandidates("recent", k + 1, base, g_names[k % kNameRing]);
  }

  // The name-table lookups that led here, newest last: caller, key text, result.
  const uint32_t lseq = g_lookup_seq.load();
  for (uint32_t i = 0; i < kLookupRing; ++i) {
    if (lseq < kLookupRing && i >= lseq) break;
    const uint32_t k = lseq >= kLookupRing ? lseq - kLookupRing + i : i;
    const Lookup& lk = g_lookups[k % kLookupRing];
    char key[64];
    Ascii(key, 64, base, lk.key_obj + 16, 48);
    REXLOG_WARN("[vm-lookup] #{} caller={:08X} key={:08X} '{}' found0={:08X} found8={:08X}",
                k + 1, lk.caller, lk.key_obj, key, lk.found0, lk.found8);
  }
}

}  // namespace

// The accessor. Cheap for every call (one guest word read); it only fills the
// log for calls made by the argument-fetch wrapper that will return NULL.
REX_HOOK_RAW(AccessElement8_82A1E620) {
  const uint32_t vm = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);  // guest LR at entry = the calling site
  __imp__AccessElement8_82A1E620(ctx, base);
  const uint32_t element = ctx.r3.u32;

  // Proves the override linked (a hook that silently does not apply would make
  // the whole run worthless), and heartbeats the name-table counter so a log
  // always says whether THAT hook has fired yet (run 059/060 had no [vm-lookup]
  // line and nothing to prove the hook was armed rather than merely unreached).
  static std::atomic<uint32_t> live{0};
  const uint32_t l = live.fetch_add(1) + 1;
  if (l <= 3) {
    REXLOG_WARN("[vm-fetch] live #{} caller={:08X} vm={:08X} index={} elem={:08X}", l, caller, vm,
                static_cast<int32_t>(index), element);
  }
  if ((l % 4096) == 0) {
    REXLOG_WARN("[vm-fetch] heartbeat #{}: name-table lookups so far = {}", l,
                g_lookup_seq.load());
  }

  if (caller != kFetchCaller) return;
  const uint32_t value = U32(base, element + 0);
  const uint32_t type = U32(base, element + 4);
  const uint32_t slot = g_fetch_seq.fetch_add(1);
  g_fetches[slot % kFetchRing] = Fetch{vm, index, element, type, value};

  // tag 2 and 7 are the two by-pointer kinds the wrapper accepts; anything else
  // makes sub_822281F8 return 0, which the caller loads through.
  if (type == 2 || type == 7) return;
  static std::atomic<uint32_t> bad{0};
  const uint32_t n = bad.fetch_add(1) + 1;
  if (n <= 6) {
    DumpFailure(n, base, vm, index, element, type, value);
  }
}

// The opcode handler that faults. Logs the first calls and keeps the newest
// dispatches for the failure dump.
REX_HOOK_RAW(sub_821C95D8) {
  const uint32_t vm = ctx.r3.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  const uint32_t slot = g_dispatch_seq.fetch_add(1);
  g_dispatches[slot % kDispatchRing] = Dispatch{caller, vm, r4, r5, r6, r7};

  static std::atomic<uint32_t> c{0};
  const uint32_t n = c.fetch_add(1) + 1;
  if (n <= 24) {
    REXLOG_WARN("[vm-call] #{} tid={} caller={:08X} vm={:08X} r4={:08X} r5={:08X} r6={:08X} "
                "r7={:08X}",
                n, ThreadId(), caller, vm, r4, r5, r6, r7);
  }
  __imp__sub_821C95D8(ctx, base);
}

// Run-057: the VM invoke helper. Every script invocation passes through here
// with the callee *value* in r4 ({value @+0, type @+4}) - the same struct shape
// the accessor returns. It fills parameters the caller omitted with type 0 (nil,
// the exact state of the crashing slot) and then dispatches through the
// handler's vtable, which is how sub_821C95D8 gets called.
//
// A type-6 callee is a *string*: the VM resolves script names that way, so this
// is where a call to something like '...MarkerExit_Gate'. can be read off
// without knowing the script. Ring + first few named calls only.
REX_HOOK_RAW(GuildChest_ProcessItem_82228830) {
  const uint32_t vm = ctx.r3.u32;
  const uint32_t callee = ctx.r4.u32;
  const uint32_t a5 = ctx.r5.u32;
  const uint32_t value = U32(base, callee + 0);
  const uint32_t type = U32(base, callee + 4);
  const uint32_t top = U32(base, vm + 8);
  const uint32_t slot = g_invoke_seq.fetch_add(1);
  g_invokes[slot % kInvokeRing] = Invoke{value, type, vm, top};

  if (type == 6) {
    const uint32_t ns = g_name_seq.fetch_add(1);
    g_names[ns % kNameRing] = value;
    static std::atomic<uint32_t> named{0};
    const uint32_t n = named.fetch_add(1) + 1;
    if (n <= 8) {
      REXLOG_WARN("[vm-name] call #{} tid={} vm={:08X} a5={:08X}", n, ThreadId(), vm, a5);
      NameCandidates("callee", n, base, value);
    }
  }
  __imp__GuildChest_ProcessItem_82228830(ctx, base);
}

// Run-059: the engine's name-table lookup.
//
// `sub_82C64018(out, key_obj, 1, 2)` is what the GUI batch builder calls with the
// composed resource key ("Data\\Worlds\\Albion\\"); its result goes on to the
// liveness test at 0x82C63040. Hooking it logs, for every lookup in the session,
// the caller (so different subsystems can be told apart), the key text and the
// found object - which is the direct answer to "is the table empty, or is this
// one entry missing?" for both the GUI key and any entity name that shares the
// table.
REX_HOOK_RAW(ProcessAndConstructAndAtomicIncrement_82C64018) {
  const uint32_t out = ctx.r3.u32;
  const uint32_t key_obj = ctx.r4.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  __imp__ProcessAndConstructAndAtomicIncrement_82C64018(ctx, base);
  const uint32_t found0 = Readable(out) ? U32(base, out) : 0;
  const uint32_t found8 = Readable(out + 8) ? U32(base, out + 8) : 0;

  const uint32_t seq = g_lookup_seq.fetch_add(1);
  g_lookups[seq % kLookupRing] = Lookup{caller, key_obj, found0, found8};

  static std::atomic<uint32_t> logged{0};
  const uint32_t n = logged.fetch_add(1) + 1;
  if (n <= 64) {
    char key[64];
    Ascii(key, 64, base, key_obj + 16, 48);
    // When something was found, a few words of it are dumped too: enough to tell
    // a real object (non-zero fields, a vtable-shaped first word) from a stub.
    const uint32_t obj = found0 ? found0 : found8;
    REXLOG_WARN("[vm-lookup] #{} caller={:08X} key={:08X} '{}' found0={:08X} found8={:08X} "
                "obj={:08X} w=[{:08X} {:08X} {:08X} {:08X}]",
                n, caller, key_obj, key, found0, found8, obj,
                obj ? U32(base, obj + 0) : 0u, obj ? U32(base, obj + 4) : 0u,
                obj ? U32(base, obj + 8) : 0u, obj ? U32(base, obj + 12) : 0u);
  } else if ((n % 256) == 0) {
    REXLOG_WARN("[vm-lookup] ... {} lookups so far (last caller={:08X})", n, caller);
  }
}
