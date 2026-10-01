# Releases

The repository publishes versioned **preview releases** for the native C++
backend. The 0.4.0 release targets self-contained C++ runtime packages for
Windows x64 and Linux x86_64; it is not a compatibility-stable SDK release.
Rust remains preserved development code and is not released in 0.4.0.

The GitHub Release is the authoritative distribution location.

The project source is maintained at
<https://github.com/ricardo-cunha/streamfind>. Releases from `v0.2.0` onward
are published from this repository. The older `v0.1.0` release is historical
and is not part of the current release line.

## Project version: {{ streamfind_version }}

The native C++ project metadata targets version **{{ streamfind_version }}**.
The latest downloadable GitHub release is `v0.4.0`.

## 0.4.0 release scope

Version 0.4.0 publishes the authoritative C++ packages only:

- Windows x86_64 ZIP;
- Linux x86_64 TGZ;
- a checksum manifest covering the published C++ archives.

The Rust backend remains in the source workspace for preservation and
development compatibility, but no Rust 0.4.0 archive will be produced.

## Latest downloadable release: 0.4.0

| Backend | Archive | Size | SHA-256 |
| --- | --- | ---: | --- |
| C++ core | [Download `streamfind-core-cpp-0.4.0-Windows-x86_64.zip`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.4.0/streamfind-core-cpp-0.4.0-Windows-x86_64.zip) | 70,954,359 bytes | `f661d4a125f425744ed22f40074629f599f811bd45604286ba240a208d2d08d6` |
| C++ core | [Download `streamfind-core-cpp-0.4.0-Linux-x86_64.tgz`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.4.0/streamfind-core-cpp-0.4.0-Linux-x86_64.tgz) | 169,407,704 bytes | `46b35bbc606fd561259a9c80f2b3855d1662af12281190e4235bb2bb83f379e0` |

The complete checksum list is available as the
[`sha256sums.txt`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.4.0/sha256sums.txt)
asset attached to the GitHub Release.

## Package contents

### C++ core archive

The C++ archives contain:

- the `streamfind_mcp.exe` or `streamfind_mcp` MCP server and C++ runtime libraries;
- public C++ headers and libraries;
- `core/catalogue.duckdb` and packaged plugin catalogues;
- the native runtime dependencies assembled by CPack.

Both archives contain the built React web app under `app/` and a `streamfind`
launcher at the package root. Running the launcher starts the local C++ service
and opens the packaged app in the default browser. The development Vite app is
also available separately when frontend source changes are being developed. See
the [Web app](web-app.md) page for the platform-specific instructions.

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
streamfind-core-cpp-0.4.0-Windows-x86_64/
├── app/
│   ├── index.html
│   └── assets/
├── streamfind.exe
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
