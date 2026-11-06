#!/usr/bin/env python3
"""Fable II deko3d shader chain, first pass: SPIR-V (dumped by the SDK's
Vulkan backend with dump_shaders) -> SPIRV-Cross Vulkan GLSL -> uam -> DKSH.

    python convert_shaders.py --spirv <dir> --out <dir> [--jobs N]

Reports how many shaders compile unchanged. Failures are grouped by uam's
first error line; those are what the deko3d GLSL rewrite step must handle
(storage buffers, descriptor sets, push constants...).

This is the offline validation half of the deko3d backend's shader pipeline;
the same steps run at runtime in the backend (SPIRV-Cross + uam linked).
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deko3d_glsl_fable

# Host tools: set SPIRV_CROSS / UAM, or put them on PATH. The repository's
# developer setup builds them under re/tools (not committed).
TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 're', 'tools')
SPIRV_CROSS = (os.environ.get('SPIRV_CROSS') or shutil.which('spirv-cross') or
               os.path.join(TOOLS, 'spirv-cross-build', 'spirv-cross'))
UAM = (os.environ.get('UAM') or shutil.which('uam') or
       os.path.join(TOOLS, 'uam-build', 'uam'))


def one(spv, out):
    stem = os.path.splitext(os.path.basename(spv))[0]
    stage = 'frag' if stem.startswith('ps_') else 'vert'
    vk = os.path.join(out, 'vk', stem + '.vk.glsl')
    glsl = os.path.join(out, 'glsl', stem + '.glsl')
    dksh = os.path.join(out, 'dksh', stem + '.dksh')
    r = subprocess.run([SPIRV_CROSS, spv, '--version', '460', '--vulkan-semantics',
                        '--relax-nan-checks', '--output', vk],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return stem, 'spirv-cross', (r.stderr or r.stdout).strip()[:300], 0
    try:
        converted = deko3d_glsl_fable.convert(open(vk, encoding='utf-8').read(), stage)
    except deko3d_glsl_fable.Unsupported as e:
        return stem, 'convert', str(e)[:300], 0
    with open(glsl, 'w', encoding='utf-8', newline='\n') as f:
        f.write(converted)
    r = subprocess.run([UAM, '-s', stage, '-o', dksh, glsl], capture_output=True, text=True)
    log = (r.stdout + r.stderr).strip()
    if r.returncode != 0 or not os.path.isfile(dksh):
        first = next((l for l in log.split('\n') if 'error' in l.lower()), log[:300])
        return stem, 'uam', first.strip()[:300], 0
    return stem, None, '', os.path.getsize(dksh)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--spirv', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    a = ap.parse_args()
    for d in ('vk', 'glsl', 'dksh'):
        os.makedirs(os.path.join(a.out, d), exist_ok=True)
    spvs = sorted(os.path.join(a.spirv, f) for f in os.listdir(a.spirv) if f.endswith('.spv'))
    if not spvs:
        raise SystemExit(f'no .spv files in {a.spirv}')
    fails = collections.defaultdict(list)
    groups = collections.Counter()
    sizes = []
    ok = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for i, (stem, reason, detail, size) in enumerate(ex.map(lambda s: one(s, a.out), spvs)):
            if reason:
                fails[reason].append((stem, detail))
                groups[re.sub(r'0x[0-9A-Fa-f]+|\d+', 'N', detail)[:180]] += 1
            else:
                ok += 1
                sizes.append(size)
            if (i + 1) % 200 == 0:
                print(f'{i + 1}/{len(spvs)}', flush=True)
    print(f'{len(spvs)} shaders: {ok} compiled, {sum(len(v) for v in fails.values())} failed')
    if sizes:
        sizes.sort()
        print(f'DKSH median {sizes[len(sizes) // 2]} bytes, max {sizes[-1]}')
    for reason, items in sorted(fails.items(), key=lambda kv: -len(kv[1])):
        print(f'\n[{reason}] {len(items)}')
        by_detail = collections.Counter(
            re.sub(r'0x[0-9A-Fa-f]+|\d+', 'N', d)[:180] for _, d in items)
        for d, n in by_detail.most_common(12):
            ex = next(s for s, dd in items
                      if re.sub(r'0x[0-9A-Fa-f]+|\d+', 'N', dd)[:180] == d)
            print(f'  {n:5}  {d}   (e.g. {ex})')


if __name__ == '__main__':
    main()
