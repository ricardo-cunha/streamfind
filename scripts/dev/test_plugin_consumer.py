#!/usr/bin/env python3
"""Build and execute an out-of-tree consumer against an installed dev kit."""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path


def run(command: list[str], cwd: Path) -> None:
    print("+", " ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    args = parser.parse_args()
    source = args.source.resolve()
    prefix = args.prefix.resolve()
    build = args.build.resolve()
    if not (source / "CMakeLists.txt").is_file():
        parser.error(f"consumer source is missing CMakeLists.txt: {source}")
    package_dir = prefix / "sdk" / "cmake" / "streamfind"
    if not (package_dir / "streamfindConfig.cmake").is_file():
        parser.error(f"developer kit CMake package is missing: {package_dir}")
    if build.exists():
        shutil.rmtree(build)
    build.mkdir(parents=True)
    run([args.cmake, "-G", "Ninja", "-S", str(source), "-B", str(build),
         f"-DCMAKE_MAKE_PROGRAM={args.ninja}", f"-DCMAKE_PREFIX_PATH={package_dir.parent}"], source)
    run([args.cmake, "--build", str(build)], source)
    executable = build / ("example_plugin.exe" if os.name == "nt" else "example_plugin")
    if not executable.is_file():
        raise SystemExit(f"consumer executable was not produced: {executable}")
    run([str(executable)], build)
    print("out-of-tree developer-kit consumer passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
