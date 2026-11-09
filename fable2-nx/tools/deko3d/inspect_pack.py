#!/usr/bin/env python3
"""Report what the DKSH pack actually needs from deko3d, so the queue is sized
from data rather than guessed.

    python3 tools/deko3d/inspect_pack.py <dksh_dir_or_pak>

Prints, per shader and in aggregate: per-warp scratch bytes (the queue must be
created with at least the maximum), GPR count, and the code/control section
sizes. uam's dksh.h layout: DkshHeader{magic, header_sz, control_sz, code_sz,
programs_off, num_programs}, then DkshProgramHeader{type, entrypoint, num_gprs,
constbuf1_off, constbuf1_sz, per_warp_scratch_sz, ...}.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

DKSH_MAGIC = 0x48534B44
PROGRAM_FIELDS = 6
SCRATCH_ALIGNMENT = 0x200


def report(name: str, blob: bytes, rows: list) -> bool:
    if len(blob) < 24 or struct.unpack_from("<I", blob, 0)[0] != DKSH_MAGIC:
        print(f"{name}: not a DKSH blob", file=sys.stderr)
        return False
    _, header_sz, control_sz, code_sz, programs_off, num_programs = struct.unpack_from(
        "<6I", blob, 0)
    if programs_off + num_programs * 24 > len(blob) or not num_programs:
        print(f"{name}: program header out of range", file=sys.stderr)
        return False
    for i in range(num_programs):
        ptype, entry, gprs, cb1_off, cb1_sz, scratch = struct.unpack_from(
            "<6I", blob, programs_off + i * 24)
        rows.append((name, ("vs", "ps", "gs", "tcs", "tes", "cs")[ptype], gprs, scratch,
                     control_sz, code_sz))
    return True


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    src = Path(sys.argv[1])
    rows: list = []
    if src.is_dir():
        for path in sorted(src.glob("*.dksh")):
            report(path.stem, path.read_bytes(), rows)
    else:
        data = src.read_bytes()
        if data[:8] not in (b"F2DKSH1\0", b"F2DKSH2\0"):
            print(f"{src} is not a F2DKSH pack", file=sys.stderr)
            return 1
        count = struct.unpack_from("<I", data, 8)[0]
        pos = 12
        entries = []
        for _ in range(count):
            name_len = struct.unpack_from("<I", data, pos)[0]
            pos += 4
            name = data[pos:pos + name_len].decode()
            pos += name_len
            bindings = struct.unpack_from("<I", data, pos)[0]
            pos += 4 + 12 * bindings
            offset, size = struct.unpack_from("<2I", data, pos)
            pos += 8
            entries.append((name, offset, size))
        for name, offset, size in entries:
            report(name, data[offset:offset + size], rows)

    if not rows:
        return 1
    scratch_max = max(r[3] for r in rows)
    gprs_max = max(r[2] for r in rows)
    code_total = sum(r[4] + r[5] for r in rows)
    for name, stage, gprs, scratch, control, code in sorted(
            rows, key=lambda r: -r[3])[:12]:
        print(f"  {name} {stage}: {gprs:4} gprs, {scratch:6} scratch/warp, "
              f"{control} + {code} bytes")
    need = ((scratch_max + SCRATCH_ALIGNMENT - 1) // SCRATCH_ALIGNMENT) * SCRATCH_ALIGNMENT
    print(f"\n{len(rows)} programs")
    print(f"per-warp scratch: max {scratch_max} -> queue size {need} ({need // 1024} KiB)")
    print(f"gprs: max {gprs_max}")
    print(f"code+control total: {code_total / 1024:.1f} KiB")
    for label, value in (("default (2 KiB)", 4 * SCRATCH_ALIGNMENT), ("64 KiB", 64 << 10)):
        print(f"  {label}: {'enough' if need <= value else 'TOO SMALL'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
