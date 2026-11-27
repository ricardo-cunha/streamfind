# Releases

## {{ streamfind_version }} release

Version **{{ streamfind_version }}** is the current native runtime release. It
provides the service-managed Result Runtime, supervised operation workers,
deuterium-aware mass-spectrometry processing, feature-network exploration, and
the browser workflow and result-viewing capabilities included in this version.

Version {{ streamfind_version }} provides native runtime packages for Windows x64 and Linux
x86_64. The packages include the built browser application, native service,
MCP server, semantic catalogues, and built-in plugins.

The GitHub Release is the authoritative distribution location.

The project source is maintained at
<https://github.com/ricardo-cunha/streamfind>.

## Project version: {{ streamfind_version }}

The native C++ project version is **{{ streamfind_version }}**.

### Upgrade notes

- MCP clients that replace a workflow with `set_workflow` must first call
  `get_workflow` and include its current `definition_revision` in the submitted
  workflow. Saves without that revision are rejected to prevent overwriting
  concurrent app or MCP edits.
- Projects are now `.sf` folders containing `project.duckdb` and `data/`.
  Earlier standalone `.duckdb` projects are not migrated or opened by this
  runtime. Open them with their matching streamfind release, export the
  workflow JSON, then create a new `.sf` project, import and validate the
  workflow, and rerun it. Keep the original database and sibling `.data`
  folder unchanged until the rebuilt project has been checked.

## Available packages

The {{ streamfind_version }} runtime distribution contains:

- Windows x86_64 ZIP;
- Linux x86_64 TGZ;
- a checksum manifest covering both runtime archives.

Runtime archives are intended for local use by the browser app, CLI, MCP
clients, native C++ applications, and external plugin/viewer development. The
runtime and embedded development payload are version-matched inside the same
archive. `streamfind-dev --version` reports the same product version as the
runtime; its provisioning protocol has a separate version identifier. The C++
ABI is not guaranteed across releases.

| Package | Archive | Platform | SHA-256 |
| --- | --- | --- | --- |
| C++ core | `streamfind-core-cpp-{{ streamfind_version }}-Windows-x86_64.zip` | Windows x64 | See `sha256sums.txt` |
| C++ core | `streamfind-core-cpp-{{ streamfind_version }}-Linux-x86_64.tgz` | Linux x86_64 | See `sha256sums.txt` |

The complete checksum list is distributed as `sha256sums.txt` alongside the
archives.

### Validation

The {{ streamfind_version }} release packages must be built from the same source revision and
pass the release checks:

- the complete CTest suite on Windows x64;
- the complete CTest suite on Linux x86_64;
- native dependency-closure validation on each platform;
- packaged MCP smoke validation against the version's published tool catalogue.

The release scripts write the packages and checksum manifest to
`tmp/release-output/` during development. Published copies and their
checksums are maintained with the GitHub Release.

## Package contents

### Runtime archive

The runtime archives contain:

- the `bin/streamfind.exe` / `bin/streamfind` CLI and `bin/streamfind-app.exe` /
  `bin/streamfind-app` browser launcher;
- `bin/streamfind-mcp.exe` or `bin/streamfind-mcp`, the MCP server, plus C++ runtime libraries;
- `core/catalogue.duckdb` and packaged plugin catalogues;
- `bin/streamfind-dev` / `bin/streamfind-dev.exe`, the matching native SDK,
  viewer SDK and verified development provisioning manifests. The developer
  dispatcher reports the same product version as the runtime archive;
- the native runtime dependencies assembled by CPack.

Both archives contain the built web app under `app/` and all executable launchers
under `bin/`. Running the app launcher starts the local C++ service and opens
the app in the default browser. See the [Web app](web-app.md) page for
platform-specific instructions.

The service is the shared control plane for the browser app, MCP, and CLI. It
owns project sessions and DuckDB metadata access, supervises operation workers,
and publishes validated results in the project's `data/` directory. Closing the
browser does not stop the service; use `bin/streamfind service status` or
`bin/streamfind service stop` to inspect or stop it.

The same archive supplies the headers, static libraries, CMake package, plugin
validator, catalogue tools and viewer SDK needed for out-of-tree development.

## Legal and attribution files

Native archives include the project notice and the C++ vendor attribution
payload:

```text
NOTICE.md
LICENSE.md
licenses/: vendor licence texts from `cpp/vendor/`

```

These files identify streamfind's licence, bundled third-party components, and
the licence terms that apply to the packaged runtime. Native archives do not
include vendor SDKs, vendor DLLs, proprietary vendor software, development-only
oracle tools, or external vendor sample files.

See the repository [`NOTICE.md`](https://github.com/ricardo-cunha/streamfind/blob/main/NOTICE.md)
for the current attribution and legal-review boundary.

## Extract and run

Extract either archive as a single directory. The MCP server can then be
launched directly by an MCP client over stdio. For the complete request flow,
see the [C++ MCP quickstart](quickstart/cpp-mcp.md).

Example C++ package layout:

```text
streamfind-core-cpp-{{ streamfind_version }}-Windows-x86_64/
├── app/
│   ├── index.html
│   └── assets/
├── bin/
│   ├── streamfind.exe
│   ├── streamfind-app.exe
│   ├── streamfind-mcp.exe
│   ├── streamfind-dev.exe
│   ├── streamfind-server.exe
│   ├── streamfind-tray.exe
│   ├── streamfind-worker.exe
│   └── streamfind.json
├── core/
│   └── catalogue.duckdb
├── plugins/
│   ├── native/
│   │   ├── native.plugin.mass_spec/
│   │   ├── native.plugin.raman/
│   │   ├── native.plugin.sensors/
│   │   └── native.plugin.statistics/
│   └── custom/
├── viewers/
│   ├── _shared/
│   ├── native/
│   └── custom/
└── dev/
    └── streamfind-dev-setup.ps1
```

## Release scope

The native archives provide the current C++ backend, MCP server, browser
application, and built-in backend plugins. They do not guarantee a stable
cross-version C++ API or ABI, or automatically install optional scientific
tools.

Optional tools such as Java and MetFrag remain explicit user-installed
components. The release packages do not download them at runtime.

## Previous release

Version **0.6.0** was the first native Windows x64 and Linux x86_64 package
release. Its packages remain available from the GitHub Release history.
