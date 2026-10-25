# Releases

## {{ streamfind_version }} release

Version **{{ streamfind_version }}** is the next native runtime release. It includes the service-owned
Result Runtime, supervised worker execution, improved operation cancellation and runtime transparency,
deuterium-aware mass-spectrometry processing, feature-network exploration, and
the browser viewer and workflow improvements developed after 0.6.0.

Version {{ streamfind_version }} provides native runtime packages for Windows x64 and Linux
x86_64. The packages include the built browser application, native service,
MCP server, semantic catalogues, and built-in domain plugins.

The GitHub Release is the authoritative distribution location.

The project source is maintained at
<https://github.com/ricardo-cunha/streamfind>.

## Project version: {{ streamfind_version }}

The native C++ project version is **{{ streamfind_version }}**.

## Available packages

The {{ streamfind_version }} runtime distribution contains:

- Windows x86_64 ZIP;
- Linux x86_64 TGZ;
- a checksum manifest covering both runtime archives.

Runtime archives are intended for local use and application integration.
Developer kits must be used with the matching runtime version; the C++ ABI is
not guaranteed across versions.

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
- packaged MCP smoke validation exposing 40 tools on each platform.

The release scripts write the packages and checksum manifest to
`tmp/release-output/` during development. Published copies and their
checksums are maintained with the GitHub Release.

## Package contents

### Runtime archive

The runtime archives contain:

- the `bin/streamfind.exe` / `bin/streamfind` CLI and `bin/streamfind-app.exe` /
  `bin/streamfind-app` browser launcher;
- the `streamfind-mcp.exe` or `streamfind-mcp` MCP server and C++ runtime libraries;
- `core/catalogue.duckdb` and packaged plugin catalogues;
- the native runtime dependencies assembled by CPack.

Both archives contain the built web app under `app/` and all executable launchers
under `bin/`. Running the app launcher starts the local C++ service and opens
the app in the default browser. See the [Web app](web-app.md) page for
platform-specific instructions.

The service is the shared control plane for the browser application, MCP, and
CLI. It owns the project session and DuckDB metadata access, supervises
operation workers, and publishes validated results in the project `.data`
sidecar. Closing the browser does not stop the service; use `bin/streamfind
service status` or `bin/streamfind service stop` to inspect or stop it.

### Developer kit

The developer kit adds the headers, static libraries, CMake package, plugin
validator, catalogue tools, and example plugin needed to build an out-of-tree
C++ plugin. It is not required for normal use of the runtime package.

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

See the repository [`NOTICE.md`](https://github.com/ricardo-cunha/streamfind/blob/dev_refactoring/NOTICE.md)
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
│   └── streamfind-server.exe
├── core/
│   └── catalogue.duckdb
└── plugins/
    └── mass_spec/
```

## Release scope

The native archives provide the current C++ backend and MCP interfaces. They do
not provide:

- a stable cross-version API or ABI guarantee;
- a public Python package;
- automatic installation of optional scientific tools;
- the separate R package or its Shiny application.

Optional tools such as Java and MetFrag remain explicit user-installed
components. The release packages do not download them at runtime.

## Previous release

Version **0.6.0** was the first native Windows x64 and Linux x86_64 package
release. Its packages remain available from the GitHub Release history.
