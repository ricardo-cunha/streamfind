#!/usr/bin/env python3
"""Validate recursive native dependencies inside a StreamFind package."""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

WINDOWS_SYSTEM = {
    "advapi32.dll", "bcrypt.dll", "comdlg32.dll", "gdi32.dll", "kernel32.dll",
    "ntdll.dll", "ole32.dll", "oleaut32.dll", "shell32.dll", "user32.dll",
    "version.dll", "ws2_32.dll", "winhttp.dll", "winmm.dll", "secur32.dll",
    "rstrtmgr.dll", "shlwapi.dll", "imm32.dll", "msimg32.dll", "comctl32.dll",
}


def run_text(command: list[str]) -> str:
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = getattr(exc, "output", "")
        raise RuntimeError(f"failed to inspect {' '.join(command)}\n{detail}") from exc


def files_by_name(root: Path) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for path in root.rglob("*"):
        if path.is_file():
            result.setdefault(path.name.lower(), path)
    return result


def pe_dependencies(path: Path, objdump: str) -> list[str]:
    output = run_text([objdump, "-p", str(path)])
    return [match.group(1).lower() for match in re.finditer(r"DLL Name:\s*([^\s]+)", output, re.I)]


def elf_dependencies(path: Path, readelf: str) -> list[str]:
    output = run_text([readelf, "-d", str(path)])
    return [match.group(1) for match in re.finditer(r"\(NEEDED\).*\[([^]]+)\]", output)]


def is_elf(path: Path) -> bool:
    try:
        return path.read_bytes()[:4] == b"\x7fELF"
    except OSError:
        return False


def is_pe(path: Path) -> bool:
    try:
        return path.read_bytes()[:2] == b"MZ"
    except OSError:
        return False


def validate_windows(root: Path) -> int:
    objdump = shutil.which("objdump") or shutil.which("C:/msys64/ucrt64/bin/objdump.exe")
    if not objdump:
        raise RuntimeError("objdump is required for Windows PE dependency validation")
    by_name = files_by_name(root)
    queue = [p for p in root.rglob("*") if p.is_file() and (p.suffix.lower() in {".exe", ".dll"}) and is_pe(p)]
    seen: set[Path] = set()
    failures: list[str] = []
    while queue:
        current = queue.pop()
        if current in seen:
            continue
        seen.add(current)
        for dependency in pe_dependencies(current, objdump):
            if dependency.startswith("api-ms-") or dependency.startswith("ext-ms-") or dependency in WINDOWS_SYSTEM:
                continue
            target = by_name.get(dependency)
            if target is None:
                failures.append(f"{current.relative_to(root)} -> {dependency}")
            elif target not in seen:
                queue.append(target)
    if failures:
        print("Missing PE dependencies:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"PE dependency closure passed: {len(seen)} binaries inspected")
    return 0


def validate_linux(root: Path) -> int:
    readelf = shutil.which("readelf")
    if not readelf:
        raise RuntimeError("readelf is required for Linux ELF dependency validation")
    by_name = files_by_name(root)
    queue = [p for p in root.rglob("*") if p.is_file() and is_elf(p)]
    seen: set[Path] = set()
    failures: list[str] = []
    system_cache: dict[str, bool] = {}
    while queue:
        current = queue.pop()
        if current in seen:
            continue
        seen.add(current)
        for dependency in elf_dependencies(current, readelf):
            target = by_name.get(dependency.lower())
            if target is not None:
                if target not in seen:
                    queue.append(target)
                continue
            if dependency not in system_cache:
                system_cache[dependency] = shutil.which("ldconfig") is not None and subprocess.run(
                    ["ldconfig", "-p"], capture_output=True, text=True, check=False
                ).stdout.find(dependency) >= 0
            if not system_cache[dependency]:
                failures.append(f"{current.relative_to(root)} -> {dependency}")
    if failures:
        print("Missing ELF dependencies:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"ELF dependency closure passed: {len(seen)} binaries inspected")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package_root", type=Path)
    parser.add_argument("--platform", choices=("windows", "linux"), default=None)
    args = parser.parse_args()
    root = args.package_root.resolve()
    if not root.is_dir():
        parser.error(f"package root does not exist: {root}")
    platform = args.platform or ("windows" if os.name == "nt" else "linux")
    try:
        return validate_windows(root) if platform == "windows" else validate_linux(root)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
