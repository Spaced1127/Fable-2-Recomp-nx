#!/usr/bin/env python3
"""Extract files from an Xbox 360 disc image (XDVDFS).

    python3 tools/extract_iso.py <image.iso> <out_dir>
    python3 tools/extract_iso.py <image.iso> <out_dir> default.xex data

The game partition is an XDVDFS filesystem; its volume descriptor sits at
sector 32 of the partition, whose offset depends on the disc format (plain
XISO, XGD1/2/3). The executable is stored compressed/encrypted in the XEX
container; this script copies files as they are and does not decrypt or
decompress them - the ReXGlue code generator does that.

With names given, only those file names (case-insensitive, any directory) are
written; the executable can be extracted alone this way:
    python3 tools/extract_iso.py game.iso game_files default.xex
"""
from __future__ import annotations

import os
import struct
import sys
import time

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
OFFSETS = (0, 0x18300000, 0xFD90000, 0x2080000)  # XISO, XGD1, XGD2, XGD3
ATTR_DIR = 0x10


def find_partition(f) -> int:
    for off in OFFSETS:
        f.seek(off + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return off
    raise SystemExit("no XDVDFS volume found - is this an Xbox disc image?")


def walk(f, part: int, sector: int, size: int, rel: str):
    if size == 0:
        return
    f.seek(part + sector * SECTOR)
    table = f.read(size)
    pos = 0
    while pos + 14 <= len(table):
        left = struct.unpack_from("<H", table, pos)[0]
        if left == 0xFFFF:  # padding to the next sector
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        _, _, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, pos)
        name = table[pos + 14 : pos + 14 + nlen].decode("latin-1")
        path = os.path.join(rel, name)
        is_dir = bool(attr & ATTR_DIR)
        yield path, start, length, is_dir
        if is_dir:
            yield from walk(f, part, start, length, path)
        pos = (pos + 14 + nlen + 3) & ~3


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    image, out = sys.argv[1], sys.argv[2]
    wanted = {w.lower() for w in sys.argv[3:]}
    os.makedirs(out, exist_ok=True)

    with open(image, "rb") as f:
        part = find_partition(f)
        f.seek(part + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        entries = [
            e for e in walk(f, part, root_sector, root_size, "")
            if not e[3] and (not wanted or os.path.basename(e[0]).lower() in wanted)
        ]
        if not entries:
            print("no matching files found", file=sys.stderr)
            return 1
        total = sum(e[2] for e in entries)
        print(f"partition 0x{part:X}: {len(entries)} file(s), {total / 2**30:.2f} GiB")

        done = 0
        t0 = time.time()
        for path, start, length, _ in entries:
            dst = os.path.join(out, path)
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            if os.path.exists(dst) and os.path.getsize(dst) == length:
                done += length  # resume
                continue
            f.seek(part + start * SECTOR)
            with open(dst + ".part", "wb") as o:
                left = length
                while left:
                    chunk = f.read(min(left, 8 << 20))
                    if not chunk:
                        raise SystemExit(f"short read in {path}")
                    o.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
            os.replace(dst + ".part", dst)
            print(f"{100 * done / total:5.1f}%  {path}", flush=True)
        print(f"done: {done / 2**30:.2f} GiB in {time.time() - t0:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
