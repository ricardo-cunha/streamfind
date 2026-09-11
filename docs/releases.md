# Releases

The repository currently publishes versioned **preview releases** for the
native C++ and Rust backends. These archives are self-contained runtime
packages for Windows x64 and Linux x86_64; they are not yet
compatibility-stable SDK releases.

The GitHub Release is the authoritative distribution location.

The project source is maintained at
<https://github.com/ricardo-cunha/streamfind>. Releases from `v0.2.0` onward
are published from this repository. The older `v0.1.0` release is historical
and is not part of the current release line.

## Project version: {{ streamfind_version }}

The native C++ and Rust project metadata targets version
**{{ streamfind_version }}**, and the latest downloadable GitHub release is
`v0.2.0`.

## Latest downloadable release: 0.2.0

| Backend | Archive | Size | SHA-256 |
| --- | --- | ---: | --- |
| C++ core | [Download `streamfind-core-cpp-0.2.0-Windows-x86_64.zip`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.2.0/streamfind-core-cpp-0.2.0-Windows-x86_64.zip) | 33,246,508 bytes | `178d4ec1ecf6de088dd986df989c1a3c5935d2bc88a3c06a41041f0457bdfb53` |
| Rust backend | [Download `streamfind-rust-0.2.0-Windows-x86_64.zip`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.2.0/streamfind-rust-0.2.0-Windows-x86_64.zip) | 21,218,964 bytes | `b33adfc34fca168cdf3fb02a034f73393c15e6b015e8587c34de1cfc587561c5` |
| C++ core | [Download `streamfind-core-cpp-0.2.0-Linux-x86_64.tgz`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.2.0/streamfind-core-cpp-0.2.0-Linux-x86_64.tgz) | 85,115,349 bytes | `2f59df6c8f332e648a1a5e6b406f1737307bb8de3d8b2937a98b05e17f6ba334` |
| Rust backend | [Download `streamfind-rust-0.2.0-Linux-x86_64.tgz`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.2.0/streamfind-rust-0.2.0-Linux-x86_64.tgz) | 30,813,449 bytes | `be6ef91b87cf37aa61cb9c1aed9da9e135e92b1b780003e6c3bbeb4c2273744a` |

The complete checksum list is available as the
[`sha256sums.txt`](https://github.com/ricardo-cunha/streamfind/releases/download/v0.2.0/sha256sums.txt)
asset attached to the GitHub Release.

## Package contents

### C++ core archive

The C++ archives contain:

- the `streamfind_mcp.exe` or `streamfind_mcp` MCP server and C++ runtime libraries;
- public C++ headers and libraries;
- `share/streamfind/catalogue.duckdb`;
- the native runtime dependencies assembled by CPack.

### Rust archive

The Rust archives contain:

- `bin/streamfind-rust-cli.exe` or `bin/streamfind-rust-cli`;
- `bin/streamfind-rust-mcp.exe` or `bin/streamfind-rust-mcp`;
- `share/streamfind/catalogue.duckdb`;
- the native runtime dependencies assembled for the package.

`share/streamfind/catalogue.duckdb` is required runtime data for both native
backends. Do not remove it from the package or distribute an MCP executable
without it.

## Legal and attribution files

Native archives should be distributed together with the project notice and
the backend-specific attribution payload:

```text
NOTICE.md
LICENSE.md
C++: vendor licence texts from `cpp/vendor/`
Rust: LICENSES.md
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

Example Rust package layout:

```text
streamfind-rust-0.2.0-Windows-x86_64/
├── bin/
│   ├── streamfind-rust-cli.exe
│   └── streamfind-rust-mcp.exe
└── share/streamfind/
    └── catalogue.duckdb
```

## Release scope

The native archives are useful for testing the current C++/Rust backends and
MCP interfaces. They do not provide:

- a stable cross-version API or ABI guarantee;
- the public Python package;
- automatic installation of optional scientific tools;
- the preserved R package or its Shiny application.

Optional tools such as Java and MetFrag remain explicit user-installed
components. The release packages do not download them at runtime.
