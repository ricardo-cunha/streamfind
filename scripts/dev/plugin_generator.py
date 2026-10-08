#!/usr/bin/env python3
"""Create an out-of-tree StreamFind backend/frontend plugin scaffold."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def identifier(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_]+", "_", value).strip("_")
    if not value or value[0].isdigit():
        value = f"plugin_{value}"
    return value.lower()


def backend_files(root: Path, domain: str, package_name: str) -> None:
    target = identifier(package_name)
    (root / "src").mkdir(parents=True, exist_ok=True)
    (root / "semantic").mkdir(parents=True, exist_ok=True)
    (root / "tests").mkdir(parents=True, exist_ok=True)
    (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project({target} VERSION 0.1.0 LANGUAGES CXX)

find_package(streamfind CONFIG REQUIRED)

add_library({target} SHARED src/plugin_entrypoint.cpp)
target_compile_features({target} PRIVATE cxx_std_20)
streamfind_add_plugin({target} {domain})
set_target_properties({target} PROPERTIES OUTPUT_NAME "streamfind_{domain}")
if(WIN32)
    set_target_properties({target} PROPERTIES PREFIX "")
endif()

install(TARGETS {target}
    RUNTIME DESTINATION plugins/{domain}
    LIBRARY DESTINATION plugins/{domain})
install(FILES plugin.json DESTINATION plugins/{domain})
install(DIRECTORY semantic/ DESTINATION plugins/{domain}/ontology)
''', encoding="utf-8")
    (root / "src" / "plugin_entrypoint.cpp").write_text('''#include <streamfind/plugin_abi.h>\n\n// Replace this scaffold with the plugin ABI entrypoint and capability bindings.\nextern "C" int streamfind_plugin_scaffold() { return STREAMFIND_PLUGIN_OK; }\n''', encoding="utf-8")
    manifest = {
        "plugin_id": domain,
        "name": f"StreamFind {domain} plugin",
        "version": "0.1.0",
        "abi_version": {"major": 1, "minor": 1},
        "sdk_compatibility": {"minimum": "0.4.1", "maximum": "0.4.x"},
        "library": {"windows-x86_64": f"streamfind_{domain}.dll", "linux-x86_64": f"libstreamfind_{domain}.so"},
        "domain": domain,
        "semantic_catalogue": "catalogue.duckdb",
        "static_composition": False,
        "platforms": ["windows-x86_64", "linux-x86_64"],
    }
    (root / "plugin.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (root / "semantic" / "README.md").write_text("Add the plugin-owned Turtle catalogue here.\n", encoding="utf-8")
    (root / "README.md").write_text(f'''# {package_name}\n\nBackend plugin for the `{domain}` domain.\n\nConfigure against an installed StreamFind developer kit:\n\n```sh\ncmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/streamfind-devkit/sdk/cmake/streamfind\ncmake --build build\n```\n''', encoding="utf-8")


def frontend_files(root: Path, package_name: str) -> None:
    package = package_name
    (root / "src").mkdir(parents=True, exist_ok=True)
    (root / "package.json").write_text(json.dumps({
        "name": package,
        "version": "0.1.0",
        "private": True,
        "type": "module",
        "scripts": {"dev": "vite", "build": "tsc --noEmit", "test": "vitest run"},
        "devDependencies": {"typescript": "^5.7.2", "vite": "^6.0.7", "vitest": "^5.0.1"},
    }, indent=2) + "\n", encoding="utf-8")
    (root / "tsconfig.json").write_text(json.dumps({"compilerOptions": {"target": "ES2022", "module": "ESNext", "moduleResolution": "Bundler", "strict": True, "noEmit": True}, "include": ["src"]}, indent=2) + "\n", encoding="utf-8")
    (root / "src" / "plugin.ts").write_text('''export interface StreamFindFrontendPlugin {\n  id: string;\n  label: string;\n  artifactTypes: string[];\n}\n\nexport const plugin: StreamFindFrontendPlugin = {\n  id: "replace-me",\n  label: "Replace me",\n  artifactTypes: [],\n};\n''', encoding="utf-8")
    (root / "README.md").write_text('''# Frontend plugin\n\nConsume the versioned StreamFind frontend/plugin contracts from the frontend workspace. Do not duplicate backend table schemas; bind viewers and editors to artifact contracts.\n''', encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(prog="streamfind-plugin")
    sub = parser.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init")
    init.add_argument("destination", type=Path)
    init.add_argument("--kind", choices=("backend", "frontend", "full"), default="full")
    init.add_argument("--domain", default=None)
    init.add_argument("--name", default=None)
    args = parser.parse_args()
    root = args.destination.resolve()
    name = args.name or root.name
    domain = identifier(args.domain or name)
    if root.exists() and any(root.iterdir()):
        parser.error(f"destination is not empty: {root}")
    root.mkdir(parents=True, exist_ok=True)
    if args.kind in ("backend", "full"):
        backend_files(root / "backend", domain, name)
    if args.kind in ("frontend", "full"):
        frontend_files(root / "frontend", f"@streamfind/{identifier(name)}")
    print(f"created {args.kind} plugin project at {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
