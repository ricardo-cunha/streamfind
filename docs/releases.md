# Releases

The repository publishes versioned **preview releases** for the native C++
backend. The 0.3.0 release targets self-contained C++ runtime packages for
Windows x64 and Linux x86_64; it is not a compatibility-stable SDK release.
Rust remains preserved development code and is not released in 0.3.0.

The GitHub Release is the authoritative distribution location.

The project source is maintained at
<https://github.com/ricardo-cunha/streamfind>. Releases from `v0.2.0` onward
are published from this repository. The older `v0.1.0` release is historical
and is not part of the current release line.

## Project version: {{ streamfind_version }}

The native C++ project metadata targets version **{{ streamfind_version }}**.
The latest downloadable GitHub release is `v0.3.0`.

## 0.3.0 release scope

Version 0.3.0 publishes the authoritative C++ packages only:

- Windows x86_64 ZIP;
- Linux x86_64 TGZ;
- a checksum manifest covering the published C++ archives.

The Rust backend remains in the source workspace for preservation and
development compatibility, but no Rust 0.3.0 archive will be produced.

## Latest downloadable release: 0.3.0

| Backend | Archive | Size | SHA-256 |
| --- | --- | ---: | --- |
| C++ core | [Download `streamfind-core-cpp-0.3.0-Windows-x86_64.zip`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.3.0/streamfind-core-cpp-0.3.0-Windows-x86_64.zip) | 70,954,362 bytes | `f52417790bf24c6ebfd194abdcb9e3e69b43eb6247a1a0a499070e40f0bbe162` |
| C++ core | [Download `streamfind-core-cpp-0.3.0-Linux-x86_64.tgz`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.3.0/streamfind-core-cpp-0.3.0-Linux-x86_64.tgz) | 167,775,348 bytes | `1b1e212067a21cb4048b67916a6de27a3de1c5cece906dffff88506fea2701a8` |

The complete checksum list is available as the
[`sha256sums.txt`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.3.0/sha256sums.txt)
asset attached to the GitHub Release.

## Package contents

### C++ core archive

The C++ archives contain:

- the `streamfind_mcp.exe` or `streamfind_mcp` MCP server and C++ runtime libraries;
- public C++ headers and libraries;
- `core/catalogue.duckdb` and packaged plugin catalogues;
- the native runtime dependencies assembled by CPack.

## Legal and attribution files

Native archives should be distributed together with the project notice and
the C++ vendor attribution payload:

```text
NOTICE.md
LICENSE.md
C++: vendor licence texts from `cpp/vendor/`

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
see the [C++ MCP quickstart](quickstart/cpp-mcp.md) or
[Rust MCP quickstart](quickstart/rust-mcp.md).

Example C++ package layout:

```text
streamfind-core-cpp-0.3.0-Windows-x86_64/
├── bin/
│   ├── streamfind_cli.exe
│   ├── streamfind_mcp.exe
│   └── streamfind_service.exe
├── core/
│   └── catalogue.duckdb
└── plugins/
    └── mass_spec/
```

## Release scope

The native archives are preview packages for the current C++ backend and MCP
interfaces. They do not provide:

- a stable cross-version API or ABI guarantee;
- the public Python package;
- automatic installation of optional scientific tools;
- the preserved R package or its Shiny application.

Optional tools such as Java and MetFrag remain explicit user-installed
components. The release packages do not download them at runtime.
