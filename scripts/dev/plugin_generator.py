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


def backend_files(root: Path, plugin: str, package_name: str) -> None:
    target = identifier(package_name)
    (root / "src").mkdir(parents=True, exist_ok=True)
    (root / "src" / "operations").mkdir(exist_ok=True)
    (root / "src" / "utils").mkdir(exist_ok=True)
    (root / "semantic").mkdir(parents=True, exist_ok=True)
    (root / "tests").mkdir(parents=True, exist_ok=True)
    (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project({target} VERSION 0.1.0 LANGUAGES CXX)

find_package(streamfind CONFIG REQUIRED)
get_filename_component(kit_root "${{streamfind_DIR}}/../../../.." ABSOLUTE)
find_program(catalogue_tool NAMES streamfind_sdk_catalogue
    PATHS "${{kit_root}}/sdk/native/bin" NO_DEFAULT_PATH REQUIRED)
file(GLOB ontology CONFIGURE_DEPENDS "${{CMAKE_CURRENT_SOURCE_DIR}}/semantic/*.ttl")
add_custom_command(OUTPUT "${{CMAKE_CURRENT_BINARY_DIR}}/catalogue.duckdb"
    COMMAND "${{CMAKE_COMMAND}}" -E env --modify "PATH=path_list_prepend:${{kit_root}}/bin" --
        "${{catalogue_tool}}"
        --core-semantic "${{kit_root}}/core/ontology"
        --plugin-semantic "${{CMAKE_CURRENT_SOURCE_DIR}}/semantic"
        --plugin {plugin} --catalogue-kind plugin
        --ontology-namespace "https://streamfind.dev/catalogue/plugins/{plugin}#"
        --output-json "${{CMAKE_CURRENT_BINARY_DIR}}/catalogue.json"
        --output-matrix "${{CMAKE_CURRENT_BINARY_DIR}}/capability_matrix.json"
        --output-db "${{CMAKE_CURRENT_BINARY_DIR}}/catalogue.duckdb"
    DEPENDS ${{ontology}}
    VERBATIM)
add_custom_target(plugin_catalogue DEPENDS "${{CMAKE_CURRENT_BINARY_DIR}}/catalogue.duckdb")
file(STRINGS "${{kit_root}}/sdk/native/include/streamfind/plugin_abi.h" abi_definitions
    REGEX "STREAMFIND_PLUGIN_ABI_(MAJOR|MINOR) +=")
foreach(definition IN LISTS abi_definitions)
    if(definition MATCHES "STREAMFIND_PLUGIN_ABI_(MAJOR|MINOR) += +([0-9]+)")
        set(abi_${{CMAKE_MATCH_1}} "${{CMAKE_MATCH_2}}")
    endif()
endforeach()
configure_file(plugin.json.in plugin.json @ONLY)

add_library({target} SHARED src/plugin_entrypoint.cpp)
add_dependencies({target} plugin_catalogue)
target_compile_features({target} PRIVATE cxx_std_20)
streamfind_add_plugin({target} {plugin})
set_target_properties({target} PROPERTIES OUTPUT_NAME "streamfind_{plugin}")
if(WIN32)
    set_target_properties({target} PROPERTIES PREFIX "")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(plugin_contract tests/plugin_contract.cpp)
    target_link_libraries(plugin_contract PRIVATE {target} streamfind::sdk)
    target_compile_features(plugin_contract PRIVATE cxx_std_20)
    add_test(NAME plugin_contract COMMAND plugin_contract)
endif()

install(TARGETS {target}
    RUNTIME DESTINATION plugins/custom/custom.plugin.{plugin}
    LIBRARY DESTINATION plugins/custom/custom.plugin.{plugin})
install(FILES "${{CMAKE_CURRENT_BINARY_DIR}}/plugin.json"
    "${{CMAKE_CURRENT_BINARY_DIR}}/catalogue.duckdb" DESTINATION plugins/custom/custom.plugin.{plugin})
install(DIRECTORY semantic/ DESTINATION plugins/custom/custom.plugin.{plugin}/ontology)
''', encoding="utf-8")
    source = '''#include <streamfind/plugin_abi.h>
#include <streamfind/sdk/plugin_host_access.hpp>
#include <cstring>
#include <exception>
#include <string_view>

namespace streamfind::example {

streamfind_plugin_status invoke(void *context, const char *request, uint32_t size,
                                streamfind_plugin_buffer *output, void *data) {
    if (!request || !output || !data) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    const auto &host = *static_cast<const streamfind_plugin_host_api *>(data);
    constexpr std::string_view response = R"({"message":"Example operation completed"})";
    try {
        sdk::PluginHostAccess access(host, context);
        access.emit_result("examplePingResult", std::string(response));
    } catch (const std::exception &error) {
        if (host.report_error) {
            const std::string_view message = error.what();
            host.report_error(message.data(), static_cast<uint32_t>(message.size()), host.user_data);
        }
        return STREAMFIND_PLUGIN_ERROR;
    }
    auto *buffer = static_cast<char *>(host.allocate(response.size(), alignof(char), host.user_data));
    if (!buffer) return STREAMFIND_PLUGIN_ERROR;
    std::memcpy(buffer, response.data(), response.size());
    output->data = buffer;
    output->size = static_cast<uint32_t>(response.size());
    return STREAMFIND_PLUGIN_OK;
}

void release(streamfind_plugin_buffer *buffer, void *data) {
    if (!buffer || !buffer->data || !data) return;
    const auto &host = *static_cast<const streamfind_plugin_host_api *>(data);
    host.deallocate(const_cast<char *>(buffer->data), buffer->size, alignof(char), host.user_data);
    *buffer = {};
}

streamfind_plugin_status bind(const streamfind_plugin_host_api *host, streamfind_plugin_api *api, void *) {
    if (!host || !api) return STREAMFIND_PLUGIN_INVALID_ARGUMENT;
    api->module_id = "@DOMAIN@.base";
    api->plugin_id = "custom.plugin.@DOMAIN@";
    api->invoke = &invoke;
    api->release_buffer = &release;
    api->user_data = const_cast<streamfind_plugin_host_api *>(host);
    return STREAMFIND_PLUGIN_OK;
}

void shutdown(streamfind_plugin_api *, void *) {}

}  // namespace streamfind::example

extern "C" STREAMFIND_PLUGIN_EXPORT streamfind_plugin_status
streamfind_plugin_get_descriptor(uint32_t major, uint32_t minor, streamfind_plugin_descriptor *descriptor) {
    if (!descriptor || major != STREAMFIND_PLUGIN_ABI_MAJOR || minor > STREAMFIND_PLUGIN_ABI_MINOR)
        return STREAMFIND_PLUGIN_INCOMPATIBLE_ABI;
    *descriptor = {};
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->abi_major = STREAMFIND_PLUGIN_ABI_MAJOR;
    descriptor->abi_minor = STREAMFIND_PLUGIN_ABI_MINOR;
    descriptor->plugin_id = "custom.plugin.@DOMAIN@";
    descriptor->plugin_version = "0.1.0";
    descriptor->register_plugin = &streamfind::example::bind;
    descriptor->shutdown_plugin = &streamfind::example::shutdown;
    return STREAMFIND_PLUGIN_OK;
}
'''
    (root / "src" / "plugin_entrypoint.cpp").write_text(source.replace("@DOMAIN@", plugin), encoding="utf-8")
    (root / "tests" / "plugin_contract.cpp").write_text(f'''#include <streamfind/plugin_abi.h>
#include <string_view>

int main() {{
    streamfind_plugin_descriptor descriptor{{}};
    const auto status = streamfind_plugin_get_descriptor(
        STREAMFIND_PLUGIN_ABI_MAJOR, STREAMFIND_PLUGIN_ABI_MINOR, &descriptor);
    if (status != STREAMFIND_PLUGIN_OK || !descriptor.plugin_id || !descriptor.register_plugin)
        return 1;
    return std::string_view(descriptor.plugin_id) == "custom.plugin.{plugin}" ? 0 : 1;
}}
''', encoding="utf-8")
    manifest = {
        "schema_version": 1,
        "plugin_id": f"custom.plugin.{plugin}",
        "name": f"StreamFind {plugin} plugin",
        "version": "0.1.0",
        "abi_version": {"major": "@abi_MAJOR@", "minor": "@abi_MINOR@"},
        "sdk_compatibility": {"minimum": "@streamfind_VERSION@", "maximum": "@streamfind_VERSION_MAJOR@.@streamfind_VERSION_MINOR@.x"},
        "library": {"windows-x86_64": f"streamfind_{plugin}.dll", "linux-x86_64": f"libstreamfind_{plugin}.so"},
        "plugin": plugin,
        "ontology_namespace": f"https://streamfind.dev/catalogue/plugins/{plugin}#",
        "semantic_catalogue": "catalogue.duckdb",
        "static_composition": False,
        "platforms": ["windows-x86_64", "linux-x86_64"],
    }
    manifest_text = json.dumps(manifest, indent=2).replace('"@abi_MAJOR@"', '@abi_MAJOR@').replace(
        '"@abi_MINOR@"', '@abi_MINOR@')
    (root / "plugin.json.in").write_text(manifest_text + "\n", encoding="utf-8")
    (root / "semantic" / "example.ttl").write_text(f'''@prefix ex: <https://streamfind.dev/catalogue/plugins/{plugin}#> .
@prefix sf: <https://streamfind.dev/vocabulary#> .
@prefix sfcore: <https://streamfind.dev/catalogue/core#> .
@prefix skos: <http://www.w3.org/2004/02/skos/core#> .

ex:{plugin}
    a sf:Plugin ;
    skos:inScheme sfcore:scheme ;
    skos:prefLabel "{plugin}" ;
    skos:definition "Example plugin plugin." .

ex:baseModule
    a sf:PluginModule ;
    sf:availableInPlugin ex:{plugin} ;
    skos:inScheme sfcore:scheme ;
    skos:prefLabel "{plugin} operations" ;
    skos:definition "Example operations provided by the {plugin} plugin." .

ex:ping
    a sf:Operation ;
    sf:operationId "{plugin}.ping" ;
    sf:availableInPlugin ex:{plugin} ;
    sf:providedByModule ex:baseModule ;
    skos:inScheme sfcore:scheme ;
    skos:prefLabel "Example ping" ;
    skos:definition "Confirm that the plugin can execute an operation." ;
    sf:category "example" ;
    sf:hasOutputPort ex:examplePingResult, sfcore:operationSuccessSignal .

ex:examplePingResult
    a sf:OperationResult ;
    sf:availableInPlugin ex:{plugin} ;
    skos:inScheme sfcore:scheme ;
    sf:dataKind "json" ;
    skos:inScheme sfcore:scheme ;
    skos:prefLabel "Example ping result" ;
    skos:definition "Message returned by the example operation." ;
    sf:type "object" .
''', encoding="utf-8")
(root / "README.md").write_text(f'''# {package_name}\n\nBackend plugin for the `{plugin}` plugin.\n\nConfigure against the SDK in an extracted StreamFind runtime archive:\n\n```sh\ncmake -S . -B build -G Ninja -Dstreamfind_DIR=/path/to/streamfind/sdk/native/cmake/streamfind\ncmake --build build\nctest --test-dir build --output-on-failure\n```\n\nFor package-managed external development, use the embedded `streamfind-dev`\nfrom the same runtime archive to build, validate, package, and install the\nplugin under `<STREAMFIND_HOME>/extensions/plugins/`.\n''', encoding="utf-8")


def frontend_files(root: Path, package_name: str, full: bool) -> None:
    root.mkdir(parents=True, exist_ok=True)
    manifest = {
        "id": package_name, "name": "Example result viewer", "version": "0.1.0",
        "apiVersion": "1.0",
    }
    if full:
        manifest["artifactContracts"] = ["examplePingResult"]
    else:
        manifest["artifactRepresentations"] = ["table"]
    (root / "plugin.manifest.json").write_text(json.dumps({
        **manifest, "entry": "./plugin.mjs",
    }, indent=2) + "\n", encoding="utf-8")
    (root / "plugin.mjs").write_text(f'''export default {{
  manifest: {json.dumps(manifest)},
  setup(api) {{
    api.registerViewer({{
      id: {json.dumps(package_name + ".summary")},
      label: 'Example result summary',
      accepts: {json.dumps(manifest.get("artifactContracts", []))},
      representations: {json.dumps(manifest.get("artifactRepresentations", []))},
      component: ({{ context }}) => {{
        const payload = context.artifact?.payload;
        if (payload === null || payload === undefined) return `No result payload is available for ${{context.resultId}}`;
        if (typeof payload === 'string') {{
          try {{ return JSON.stringify(JSON.parse(payload), null, 2); }}
          catch {{ return payload; }}
        }}
        return JSON.stringify(payload, null, 2);
      }},
    }});
  }},
}};
''', encoding="utf-8")
    (root / "README.md").write_text(
        "# Frontend plugin\n\nHost the module at an allowed origin and add its manifest entry "
        "to the application's plugins.json. Resolve entry relative to that deployment.\n",
        encoding="utf-8",
    )


def viewer_files(root: Path, package_name: str, contract: str) -> None:
    """Create an independently buildable React viewer package."""
    root.mkdir(parents=True, exist_ok=True)
    css_asset_name = package_name.rsplit('/', 1)[-1] + '.css'
    package = {
        "name": package_name,
        "private": True,
        "version": "0.1.0",
        "type": "module",
        "scripts": {"build": "vite build", "test": "vitest run"},
        "peerDependencies": {
            "@streamfind/viewer-sdk": "^1.0.0",
            "react": "^18.3.1",
            "react-dom": "^18.3.1",
        },
        "peerDependenciesMeta": {
            "@streamfind/viewer-sdk": {"optional": True},
        },
        "devDependencies": {
            "@vitejs/plugin-react": "^5.1.0",
            "@types/react": "^19.0.0",
            "typescript": "^5.7.2",
            "vite": "^6.0.7",
            "vitest": "^5.0.1",
        },
    }
    (root / "package.json").write_text(json.dumps(package, indent=2) + "\n", encoding="utf-8")
    (root / "package-lock.json").write_text(
        json.dumps({"name": package_name, "version": "0.1.0", "lockfileVersion": 3, "requires": True, "packages": {"": package}}, indent=2)
        + "\n",
        encoding="utf-8",
    )
    (root / "plugin.manifest.json").write_text(json.dumps({
        "schemaVersion": 1,
        "kind": "viewer",
        "id": package_name,
        "name": "Example StreamFind viewer",
        "version": "0.1.0",
        "apiVersion": "1.0",
        "activation": "optional",
        "source": "src/index.tsx",
        "artifactContracts": [contract],
    }, indent=2) + "\n", encoding="utf-8")
    (root / "tsconfig.json").write_text(json.dumps({
        "compilerOptions": {
            "target": "ES2022", "module": "ESNext", "moduleResolution": "Bundler",
            "strict": True, "jsx": "react-jsx", "noEmit": True, "skipLibCheck": True,
        },
        "include": ["src"],
    }, indent=2) + "\n", encoding="utf-8")
    (root / "vite.config.ts").write_text("""import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  build: {
    manifest: true,
    lib: { entry: 'src/index.tsx', formats: ['es'], fileName: () => 'viewer.mjs' },
    rollupOptions: { external: ['react', 'react-dom', 'react/jsx-runtime', '@streamfind/viewer-sdk'] },
  },
});
""", encoding="utf-8")
    (root / "src" / "index.tsx").parent.mkdir(parents=True)
    (root / "src" / "index.tsx").write_text(f"""import {{ useState }} from 'react';
import type {{ FrontendPlugin }} from '@streamfind/viewer-sdk';
import './viewer.css';

const contract = {json.dumps(contract)};

function ExampleViewer({{ context }}: {{ context: {{ resultId: string }} }}) {{
  const [selected, setSelected] = useState(false);
  const assetUrl = new URL('./{css_asset_name}', import.meta.url).href;
  return <button data-asset-url={{assetUrl}} onClick={{() => setSelected(!selected)}}>
    {{selected ? 'Selected' : 'Select'}} {{context.resultId}}
  </button>;
}}

const plugin: FrontendPlugin = {{
  manifest: {{
    schemaVersion: 1, kind: 'viewer', id: {json.dumps(package_name)},
    name: 'Example StreamFind viewer', version: '0.1.0', apiVersion: '1.0',
    artifactContracts: [contract],
  }},
  setup(api) {{
    api.registerViewer({{ id: {json.dumps(package_name + '.viewer')}, label: 'Example viewer', accepts: [contract], component: ExampleViewer }});
  }},
}};

export default plugin;
""", encoding="utf-8")
    (root / "src" / "streamfind-viewer-sdk.d.ts").write_text("""declare module '@streamfind/viewer-sdk' {
  export type FrontendPlugin = {
    manifest: {
      schemaVersion: 1;
      kind: 'viewer';
      id: string;
      name: string;
      version: string;
      apiVersion: '1.0';
      artifactContracts?: string[];
      artifactRepresentations?: string[];
    };
    setup(api: {
      registerViewer(viewer: {
        id: string;
        label: string;
        accepts: string[];
        representations?: string[];
        component: (props: { context: { resultId: string } }) => import('react').ReactNode;
      }): void;
    }): void;
  };
}
""", encoding="utf-8")
    (root / "src" / "viewer.css").write_text(".streamfind-example-viewer { color: var(--accent); }\n", encoding="utf-8")
    (root / "src" / "index.test.tsx").write_text("""import { describe, expect, it } from 'vitest';
import plugin from './index';

describe('viewer scaffold', () => {
  it('exports a versioned viewer plugin', () => {
    expect(plugin.manifest.kind).toBe('viewer');
    expect(plugin.manifest.apiVersion).toBe('1.0');
  });
});
""", encoding="utf-8")
    (root / "README.md").write_text(
        "# StreamFind viewer\n\nInstall the released `@streamfind/viewer-sdk` and React peer dependencies, then run `npm ci` and `npm run build`. The generated ESM bundle keeps React, JSX runtime and the viewer SDK external so the packaged host supplies one compatible instance.\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(prog="streamfind-plugin")
    sub = parser.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init")
    init.add_argument("destination", type=Path)
    init.add_argument("--kind", choices=("backend", "frontend", "viewer", "full"), default="full")
    init.add_argument("--plugin", default=None)
    init.add_argument("--name", default=None)
    init.add_argument("--contract", default="https://streamfind.dev/catalogue/plugins/example#examplePingResult")
    args = parser.parse_args()
    root = args.destination.resolve()
    name = args.name or root.name
    plugin = identifier(args.plugin or name)
    if root.exists() and any(root.iterdir()):
        parser.error(f"destination is not empty: {root}")
    root.mkdir(parents=True, exist_ok=True)
    if args.kind in ("backend", "full"):
        backend_files(root / "backend", plugin, name)
    if args.kind in ("frontend", "full"):
        frontend_files(root / "frontend", f"@streamfind/{identifier(name)}", args.kind == "full")
    if args.kind == "viewer":
        viewer_files(root, f"@streamfind/{identifier(name)}-viewer", args.contract)
    print(f"created {args.kind} plugin project at {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
