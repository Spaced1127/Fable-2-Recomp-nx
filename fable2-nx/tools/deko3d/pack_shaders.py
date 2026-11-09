#!/usr/bin/env python3
"""Pack converted DKSH shaders into the archive the Switch NRO loads.

    python3 tools/deko3d/pack_shaders.py <dksh_dir> <output.pak>

The input is the `dksh/` folder produced by convert_shaders.py; file stems are
the keys (`ps_<ucode_hash>_<modification>` / `vs_...`), matching what the
Vulkan backend's dump_shaders aid writes.

Format (little-endian):

    "F2DKSH1\0"    8 bytes magic
    u32             entry count
    per entry:
      u32           name length (bytes, no NUL)
      name bytes
      u32           data offset from the start of the file
      u32           data size
    then the DKSH blobs

The pack is derived from the user's own game shaders and must never be
committed; it lives beside the game files on the SD card.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

MAGIC = b"F2DKSH1\0"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dksh_dir")
    ap.add_argument("output")
    a = ap.parse_args()

    src = Path(a.dksh_dir)
    files = sorted(p for p in src.iterdir() if p.suffix == ".dksh")
    if not files:
        print(f"no .dksh files in {src}", file=sys.stderr)
        return 1

    entries = [(p.stem.encode("utf-8"), p.read_bytes()) for p in files]

    # Lay out: magic + count + directory, then blobs.
    dir_size = 4 + 4
    for name, data in entries:
        dir_size += 4 + len(name) + 4 + 4
    offset = 8 + dir_size

    out = Path(a.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(entries)))
        # First pass writes the directory with placeholder offsets; we know the
        # layout already, so compute offsets directly.
        current = offset
        for name, data in entries:
            f.write(struct.pack("<I", len(name)))
            f.write(name)
            f.write(struct.pack("<II", current, len(data)))
            current += len(data)
        for _, data in entries:
            f.write(data)

    total = sum(len(d) for _, d in entries)
    print(f"packed {len(entries)} shaders, {total / 1024:.1f} KB -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
