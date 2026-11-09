#!/usr/bin/env python3
"""Pack converted DKSH shaders into the archive the Switch NRO loads.

    python3 tools/deko3d/pack_shaders.py <dksh_dir> <output.pak> [--glsl <dir>]

The input is the `dksh/` folder produced by convert_shaders.py; file stems are
the keys (`ps_<ucode_hash>_<modification>` / `vs_...`), matching what the
Vulkan backend's dump_shaders aid writes. The sibling `glsl/` folder (same
stems) is read for the per-shader texture/sampler binding table, because that
is where the converter's binding numbers and the Xenos names live.

Format (little-endian):

    "F2DKSH2\0"    8 bytes magic
    u32             entry count
    per entry:
      u32           name length (bytes, no NUL)
      name bytes
      u32           sampler binding count
      per binding, 12 bytes:
        u16         deko3d texture binding index
        u8          texture fetch constant [0-31]
        u8          sampler fetch constant [0-31]
        u8          dimension (0 = 2D array, 1 = cube, 2 = 3D)
        u8          signed (0/1)
        u8          mag filter (xenos::TextureFilter)
        u8          min filter
        u8          mip filter
        u8          aniso filter (xenos::AnisoFilter, 7 = use fetch const)
        u8          reserved
      u32           data offset from the start of the file
      u32           data size
    then the DKSH blobs

The binding table is needed because the deko3d backend cannot recover it from
the DKSH: uam drops the SPIR-V names, and the descriptor sets the Vulkan
backend numbers per stage collapse into deko3d's single texture namespace.

The pack is derived from the user's own game shaders and must never be
committed; it lives beside the game files on the SD card.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

MAGIC = b"F2DKSH2\0"

DIMENSIONS = {"2d": 0, "cube": 1, "3d": 2}
# xenos::TextureFilter, in the order spirv_translator_fetch.cpp names them.
FILTERS = {"p": 0, "l": 1, "b": 2, "f": 3}

BINDING = re.compile(
    r"layout\(binding\s*=\s*(\d+)\)\s*uniform\s+(?P<type>\w+)\s+"
    r"dk_xe_texture(?P<tex>\d+)_(?P<dim>2d|cube|3d)_(?P<sign>[us])"
    r"_xe_sampler(?P<samp>\d+)_(?P<mag>[plbf])(?P<min>[plbf])(?P<mip>[plbf])"
    r"(?:_a(?P<aniso>\d+))?\s*;")


def aniso_from_name(value: str | None) -> int:
    """`_a0` is kDisabled, `_aN` is 1 << (aniso - 1); absent is kUseFetchConst."""
    if value is None:
        return 7
    ratio = int(value)
    if ratio <= 1:
        return 0
    aniso = 1
    while (1 << (aniso - 1)) < ratio and aniso < 5:
        aniso += 1
    return aniso


def parse_bindings(glsl_path: Path) -> list[bytes]:
    text = glsl_path.read_text(encoding="utf-8")
    records = []
    for m in BINDING.finditer(text):
        dimension = DIMENSIONS[m.group("dim")]
        # The converter turns every texture/sampler pair into one combined
        # sampler; only the dimension decides the image view type.
        glsl_type = m.group("type")
        if dimension == 0 and not glsl_type.startswith(("sampler2D", "texture2D")):
            raise SystemExit(f"{glsl_path.name}: {glsl_type} does not match a 2D texture")
        records.append(struct.pack(
            "<HBBBBBBBBBB",
            int(m.group(1)),
            int(m.group("tex")),
            int(m.group("samp")),
            dimension,
            1 if m.group("sign") == "s" else 0,
            FILTERS[m.group("mag")],
            FILTERS[m.group("min")],
            FILTERS[m.group("mip")],
            aniso_from_name(m.group("aniso")),
            0,
            0,
        ))
    return records


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dksh_dir")
    ap.add_argument("output")
    ap.add_argument("--glsl", help="converted GLSL folder (default: ../glsl next to dksh_dir)")
    a = ap.parse_args()

    src = Path(a.dksh_dir)
    glsl_dir = Path(a.glsl) if a.glsl else src.parent / "glsl"
    files = sorted(p for p in src.iterdir() if p.suffix == ".dksh")
    if not files:
        print(f"no .dksh files in {src}", file=sys.stderr)
        return 1

    entries = []
    samplers_total = 0
    missing_glsl = 0
    for p in files:
        name = p.stem.encode("utf-8")
        glsl = glsl_dir / (p.stem + ".glsl")
        if glsl.is_file():
            records = parse_bindings(glsl)
        else:
            records = []
            missing_glsl += 1
        samplers_total += len(records)
        entries.append((name, records, p.read_bytes()))

    # Lay out: magic + count + directory, then blobs.
    dir_size = 4
    for name, records, data in entries:
        dir_size += 4 + len(name) + 4 + 12 * len(records) + 4 + 4
    current = 8 + dir_size

    out = Path(a.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(entries)))
        for name, records, data in entries:
            f.write(struct.pack("<I", len(name)))
            f.write(name)
            f.write(struct.pack("<I", len(records)))
            for record in records:
                f.write(record)
            f.write(struct.pack("<II", current, len(data)))
            current += len(data)
        for _, _, data in entries:
            f.write(data)

    total = sum(len(d) for _, _, d in entries)
    print(f"packed {len(entries)} shaders, {total / 1024:.1f} KB, "
          f"{samplers_total} texture bindings -> {out}")
    if missing_glsl:
        print(f"warning: {missing_glsl} shaders had no converted GLSL; "
              f"they get an empty binding table", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
