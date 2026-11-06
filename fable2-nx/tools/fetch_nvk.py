#!/usr/bin/env python3
"""Fetch the pinned Mesa NVK build for Nintendo Switch.

NVK is Mesa's Vulkan driver. The archive is a public, MIT-licensed build of
Mesa 25.0.7 for Horizon; it contains libvulkan.a, the Vulkan headers and the
driver NOTICE. It contains no game code.

    python3 tools/fetch_nvk.py --dest third_party/nvk/extracted

The release URL and SHA-256 are pinned so every build uses the same driver.
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

VERSION = "nvk-switch-mesa-25.0.7-r1"
URL = (
    "https://github.com/jhuz0/nvk-switch/releases/download/"
    f"{VERSION}/nvk-switch-25.0.7.tar.xz"
)
SHA256 = "4ab244f982dc078aaff8c58640d7cf8a0f3ab43bbf2daadfd00c02fbf352af86"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dest", default="third_party/nvk/extracted")
    ap.add_argument("--cache", default="third_party/nvk", help="download cache directory")
    a = ap.parse_args()

    dest = Path(a.dest).resolve()
    cache = Path(a.cache).resolve()
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f"nvk-switch-25.0.7.tar.xz"

    if not archive.exists() or sha256(archive) != SHA256:
        print(f"downloading {URL}")
        with urllib.request.urlopen(URL) as response, tempfile.NamedTemporaryFile(
            dir=cache, delete=False
        ) as tmp:
            shutil.copyfileobj(response, tmp)
            tmp_path = Path(tmp.name)
        if sha256(tmp_path) != SHA256:
            tmp_path.unlink(missing_ok=True)
            print("SHA-256 mismatch", file=sys.stderr)
            return 1
        tmp_path.replace(archive)
    else:
        print(f"using cached {archive}")

    print(f"extracting to {dest}")
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:xz") as tar:
        tar.extractall(dest)
    print(f"done: {dest / 'lib' / 'libvulkan.a'}")
    print("NVK is MIT-licensed; see NOTICE.md in the extracted folder.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
