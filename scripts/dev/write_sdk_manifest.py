#!/usr/bin/env python3
"""Finalize the installed native SDK manifest with deterministic payload hashes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    manifest = args.root / "sdk" / "native" / "sdk-manifest.json"
    if not manifest.is_file():
        parser.error(f"installed SDK manifest is missing: {manifest}")
    data = json.loads(manifest.read_text(encoding="utf-8"))
    data["package_hashes"] = {
        path.relative_to(args.root).as_posix(): sha256(path)
        for path in sorted(manifest.parent.rglob("*"))
        if path.is_file() and path != manifest
    }
    manifest.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
