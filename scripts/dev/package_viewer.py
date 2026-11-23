"""Package a built StreamFind viewer with a manifest and locked checksums."""

import argparse
import json
import shutil
from pathlib import Path


def fingerprint(value: bytes) -> str:
    result = 1469598103934665603
    for byte in value:
        result ^= byte
        result = (result * 1099511628211) & ((1 << 64) - 1)
    return f"{result:016x}"


def main() -> int:
    parser = argparse.ArgumentParser(prog="streamfind-viewer-package")
    parser.add_argument("project", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    project = args.project.resolve()
    output = args.output.resolve()
    manifest_path = project / "plugin.manifest.json"
    dist = project / "dist"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("kind") != "viewer" or not manifest.get("id") or not manifest.get("version"):
        parser.error("plugin.manifest.json must describe a versioned viewer")
    bundle = dist / "viewer.mjs"
    if not bundle.is_file():
        parser.error(f"built viewer bundle is missing: {bundle}")
    if output.exists():
        parser.error(f"output already exists: {output}")
    output.mkdir(parents=True)
    shutil.copy2(manifest_path, output / "plugin.manifest.json")
    shutil.copytree(dist, output / "dist")
    manifest["entry"] = "dist/viewer.mjs"
    manifest["css"] = sorted(
        f"dist/{path.name}" for path in (dist.glob("*.css")) if path.is_file()
    )
    (output / "plugin.manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    files = {}
    for path in sorted(output.rglob("*")):
        if not path.is_file() or path.name in {"checksums.json", "activation.json"}:
            continue
        relative = path.relative_to(output).as_posix()
        files[relative] = "streamfind:" + fingerprint(path.read_bytes())
    digest = "streamfind:" + fingerprint(json.dumps(files, separators=(",", ":")).encode())
    (output / "checksums.json").write_text(
        json.dumps({"schema_version": 1, "package_digest": digest, "files": files}, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"packaged viewer {manifest['id']} {manifest['version']} at {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
