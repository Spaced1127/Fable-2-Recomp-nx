#!/usr/bin/env python3
"""d3d_match restricted to a candidate address window (Fable's D3D block).

Same token/shape approach as sa2-nx's d3d_match.py, but the candidate set is
limited to function starts inside --ours-range (e.g. 82B80000-82BB0000), and
the top N candidates per named reference function are printed.
"""
import argparse, collections, difflib, glob, math, os, re, struct

BASE = 0x82000000
MAX_INSNS = 600


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
    if op in (16, 18):
        return (op, x & 1, rd if op == 16 else 0, ra if op == 16 else 0)
    if op in (19, 31, 63):
        return (op, (x >> 1) & 0x3FF, rd, ra, rb)
    if op == 59:
        return (op, (x >> 1) & 0x1F, rd, ra, rb)
    if op == 30:
        return (op, (x >> 2) & 7, rd, ra)
    if op == 4:
        return (op, x & 0x7FF, rd)
    return (op, rd, ra)


def functions(image, starts):
    out = {}
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else s + 4 * MAX_INSNS
        n = min((e - s) // 4, MAX_INSNS)
        off = s - BASE
        if off < 0 or off + 4 * n > len(image):
            continue
        out[s] = [token(struct.unpack_from('>I', image, off + 4 * k)[0]) for k in range(n)]
    return out


def histogram(tokens):
    return collections.Counter(t[0] if t[0] not in (19, 31, 59, 63) else (t[0], t[1]) for t in tokens)


def cosine(a, b):
    num = sum(a[k] * b[k] for k in a if k in b)
    den = math.sqrt(sum(v * v for v in a.values())) * math.sqrt(sum(v * v for v in b.values()))
    return num / den if den else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref-image', required=True)
    ap.add_argument('--ref-gen', required=True)
    ap.add_argument('--ref-names', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--gen', required=True)
    ap.add_argument('--ours-range', required=True, help='START-END hex, e.g. 82B80000-82BB0000')
    ap.add_argument('--top', type=int, default=4)
    a = ap.parse_args()
    lo, hi = [int(x, 16) for x in a.ours_range.split('-')]

    names = {int(m.group(1), 16): m.group(2) for m in
             re.finditer(r'(0x[0-9A-Fa-f]+)\s*=\s*\{\s*name\s*=\s*"([^"]+)"', open(a.ref_names).read())}
    ref = functions(open(a.ref_image, 'rb').read(), function_starts(a.ref_gen))
    all_ours = function_starts(a.gen)
    cand_starts = [s for s in all_ours if lo <= s < hi]
    ours = functions(open(a.image, 'rb').read(), cand_starts)
    ours_hist = {s: histogram(t) for s, t in ours.items()}
    print(f'reference functions: {len(ref)}; candidates in {a.ours_range}: {len(ours)}')
    for addr, name in sorted(names.items(), key=lambda t: t[1]):
        rt = ref.get(addr)
        if not rt:
            print(f'{name:40} reference 0x{addr:08X}: not a function start')
            continue
        rh = histogram(rt)
        cands = [(cosine(rh, ours_hist[s]), s) for s, t in ours.items()
                 if 0.6 <= len(t) / len(rt) <= 1.6]
        cands.sort(reverse=True)
        scored = []
        for _, s in cands[:80]:
            sm = difflib.SequenceMatcher(None, rt, ours[s], autojunk=False)
            scored.append((sm.ratio(), s, len(ours[s])))
        scored.sort(reverse=True)
        print(f'\n{name}  (ref 0x{addr:08X}, {len(rt)} ins)')
        for ratio, s, n in scored[:a.top]:
            print(f'   0x{s:08X}  ratio {ratio:.3f}  ({n} ins)')


if __name__ == '__main__':
    main()
