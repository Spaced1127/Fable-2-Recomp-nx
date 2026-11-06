#!/usr/bin/env python3
"""Release guard: fail if game material or generated game code is tracked.

Run from the repository root (after `git add`, or in CI after checkout). It
checks the files Git knows about, so untracked extracts and build outputs are
ignored. Game data must never be committed: the release is a builder + tools.

    python3 tools/check_no_game_data.py
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

# Formats that are game content (or containers of it).
FORBIDDEN_EXT = {
    ".iso", ".xex", ".bik", ".pac", ".sbk", ".bnk", ".gdb", ".babel",
    ".nro", ".nsp", ".xci", ".wad", ".xwb", ".xma", ".xsb", ".xsc",
}

FORBIDDEN_NAMES = {
    "default.xex",
    "default_image.bin",
}

FORBIDDEN_PARTS = {
    "game_files",
    "port/generated",
    "port/build",
    "sdk/out",
    "third_party/nvk",
    "saves",
    "cache",
    "logs",
    "crash_reports",
    "stage",
    "re",
}
MAX_FILE_BYTES = 50 * 1024 * 1024
WARN_FILE_BYTES = 10 * 1024 * 1024

# Documentation screenshots of the running port are allowed (never game assets
# used by the build).
SCREENSHOT_DIR = "docs/screenshots/"


def tracked_files(root: Path) -> list[Path]:
    try:
        out = subprocess.check_output(
            ["git", "ls-files", "-z"], cwd=root, stderr=subprocess.DEVNULL
        )
        names = [n for n in out.decode("utf-8", "surrogateescape").split("\0") if n]
        return [root / n for n in names]
    except (OSError, subprocess.CalledProcessError):
        return [p for p in root.rglob("*") if p.is_file() and ".git" not in p.parts]


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    errors: list[str] = []
    warnings: list[str] = []
    checked = 0
    for path in tracked_files(root):
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        checked += 1
        lowered = rel.lower()
        if path.name.lower() in FORBIDDEN_NAMES:
            errors.append(f"game executable: {rel}")
            continue
        if path.suffix.lower() in FORBIDDEN_EXT:
            errors.append(f"game-content extension {path.suffix}: {rel}")
            continue
        if any(lowered == part or lowered.startswith(part + "/") for part in FORBIDDEN_PARTS):
            errors.append(f"forbidden path: {rel}")
            continue
        size = path.stat().st_size
        if size > MAX_FILE_BYTES:
            errors.append(f"file over {MAX_FILE_BYTES // 2**20} MB: {rel} ({size} bytes)")
        elif size > WARN_FILE_BYTES:
            warnings.append(f"large file ({size} bytes): {rel}")
        if (
            path.suffix.lower() in {".png", ".jpg", ".jpeg"}
            and not lowered.startswith(SCREENSHOT_DIR)
            and not lowered.startswith("sdk/")
        ):
            warnings.append(f"image outside {SCREENSHOT_DIR}: {rel}")

    print(f"checked {checked} tracked file(s)")
    for warning in warnings:
        print(f"warning: {warning}")
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    if errors:
        print(f"\n{len(errors)} error(s): the repository must not contain game data.", file=sys.stderr)
        return 1
    print("ok: no game data tracked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
