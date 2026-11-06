#!/usr/bin/env python3
"""Exact masked-token prefix match of D3D functions between two XDK 2.0.7645
games (Banjo-Kazooie: Nuts & Bolts -> Fable II).

Fable and N&B link the same Direct3D build, so a function's instruction
sequence is identical up to immediates and branch displacements. This script
normalises each instruction to (primary opcode, extended opcode, register
fields) - immediates/displacements removed - and finds Fable functions whose
prefix equals a named N&B function's sequence.

    python d3d_match_exact.py --ref-image banjo_image.bin --ref-gen re/banjo/generated/default \
        --ref-names re/banjo_d3d_names.toml --image port/default_image.bin \
        --gen port/generated/default [--ours-range 82B80000-82BB0000]
"""
import argparse
import glob
import os
import re
import struct

BASE = 0x82000000
MAX_INSNS = 1200


def function_starts(gen):
    starts = set()
    for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        for m in re.finditer(r'^DEFINE_REX_FUNC\((?:sub_)?([0-9A-Fa-f]{8})\)',
                             open(path, errors='replace').read(), re.M):
            starts.add(int(m.group(1), 16))
    return sorted(starts)


def token(x):
    op = x >> 26
    rd, ra, rb = (x >> 21) & 31, (x >> 16) & 31, (x >> 11) & 31
    if op in (16, 18):  # bc / b: link bit, BO/BI (opcode shape), no displacement
        if op == 16:
            return (16, x & 1, (x >> 21) & 31, (x >> 16) & 31)
        return (18, x & 1)
    if op in (19, 31, 63):
        return (op, (x >> 1) & 0x3FF, rd, ra, rb)
    if op == 59:
        return (op, (x >> 1) & 0x1F, rd, ra, rb)
    if op == 30:
        return (op, (x >> 2) & 7, rd, ra)
    if op == 4:
        return (op, x & 0x7FF, rd)
    return (op, rd, ra)  # D-form: immediates/displacements removed


def tokens(image, start, end_hint):
    n = min(max((end_hint - start) // 4, 0), MAX_INSNS)
    off = start - BASE
    if off < 0 or off + 4 * n > len(image):
        return []
    return [token(struct.unpack_from('>I', image, off + 4 * k)[0]) for k in range(n)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref-image', required=True)
    ap.add_argument('--ref-gen', required=True)
    ap.add_argument('--ref-names', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--gen', required=True)
    ap.add_argument('--ours-range', default='82000000-83620000')
    ap.add_argument('--min-prefix', type=int, default=8)
    a = ap.parse_args()
    lo, hi = [int(x, 16) for x in a.ours_range.split('-')]

    names = {int(m.group(1), 16): m.group(2) for m in
             re.finditer(r'(0x[0-9A-Fa-f]+)\s*=\s*\{\s*name\s*=\s*"([^"]+)"', open(a.ref_names).read())}
    ref_img = open(a.ref_image, 'rb').read()
    our_img = open(a.image, 'rb').read()
    ref_starts = function_starts(a.ref_gen)
    ref_next = {s: (ref_starts[i + 1] if i + 1 < len(ref_starts) else s + 4 * MAX_INSNS)
                for i, s in enumerate(ref_starts)}
    our_starts = [s for s in function_starts(a.gen) if lo <= s < hi]
    our_next = {s: (our_starts[i + 1] if i + 1 < len(our_starts) else s + 4 * MAX_INSNS)
                for i, s in enumerate(our_starts)}
    our_tokens = {s: tokens(our_img, s, our_next[s]) for s in our_starts}

    for addr, name in sorted(names.items(), key=lambda t: t[1]):
        rt = tokens(ref_img, addr, ref_next.get(addr, addr + 4 * MAX_INSNS))
        if len(rt) < a.min_prefix:
            print(f'{name}: reference function too short / missing at 0x{addr:08X}')
            continue
        best = []
        for s, ct in our_tokens.items():
            if len(ct) < len(rt):
                continue
            if ct[:len(rt)] == rt:
                best.append((len(ct), s))
        if best:
            for n, s in sorted(best)[:4]:
                print(f'{name:40} 0x{addr:08X} ({len(rt)} ins) -> 0x{s:08X} EXACT ({n} ins)')
        else:
            # longest common prefix among candidates
            bl, bs = 0, 0
            for s, ct in our_tokens.items():
                n = 0
                for x, y in zip(rt, ct):
                    if x != y:
                        break
                    n += 1
                if n > bl:
                    bl, bs = n, s
            print(f'{name:40} 0x{addr:08X} ({len(rt)} ins) -> 0x{bs:08X} prefix {bl}/{len(rt)}')


if __name__ == '__main__':
    main()
