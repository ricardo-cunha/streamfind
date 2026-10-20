# Releases

## 0.6.0 development notes

The next release is being prepared as **0.6.0**. The release checklist should
include the current browser viewer improvements, mass-spectrometry debugging
and annotation work, workflow node improvements, and the unified operation
cancellation design documented in [Operation cancellation](architecture.md#operation-cancellation).

The 0.6.0 release build is not published until its native, frontend, and
packaged-runtime validation has passed. The release archives and checksums are
generated only by the release scripts.

Version 0.6.0 provides native runtime packages for Windows x64 and Linux
x86_64, plus matching C++ plugin developer kits.

The GitHub Release is the authoritative distribution location.

The project source is maintained at
<https://github.com/ricardo-cunha/streamfind>.

## Project version: {{ streamfind_version }}

The native C++ project version is **{{ streamfind_version }}**.

## Available packages

Version 0.6.0 contains:

- Windows x86_64 ZIP;
- Linux x86_64 TGZ;
- an optional C++ plugin developer-kit archive;
- a checksum manifest covering the published C++ archives.

Runtime archives are intended for local use and application integration.
Developer kits must be used with the matching runtime version; the C++ ABI is
not guaranteed across versions.

| Package | Archive | Platform | SHA-256 |
| --- | --- | --- | --- |
| C++ core | `streamfind-core-cpp-0.6.0-Windows-x86_64.zip` | Windows x64 | See `sha256sums.txt` |
| C++ core | `streamfind-core-cpp-0.6.0-Linux-x86_64.tgz` | Linux x86_64 | See `sha256sums.txt` |
| Plugin developer kit | `streamfind-plugin-dev-0.6.0-Windows-x86_64.zip` | Windows x64 | See `sha256sums.txt` |
| Plugin developer kit | `streamfind-plugin-dev-0.6.0-Linux-x86_64.tar.gz` | Linux x86_64 | See `sha256sums.txt` |

The complete checksum list is distributed as `sha256sums.txt` alongside the
archives.

## Package contents

### Runtime archive

The runtime archives contain:

- the `streamfind_mcp.exe` or `streamfind_mcp` MCP server and C++ runtime libraries;
- `core/catalogue.duckdb` and packaged plugin catalogues;
- the native runtime dependencies assembled by CPack.

Both archives contain the built web app under `app/` and a `streamfind` launcher
at the package root. Running the launcher starts the local C++ service and opens
the app in the default browser. See the [Web app](web-app.md) page for
platform-specific instructions.

### Developer kit

The developer kit adds the headers, static libraries, CMake package, plugin
validator, catalogue tools, and example plugin needed to build an out-of-tree
C++ plugin. It is not required for normal use of the runtime package.

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
see the [C++ MCP quickstart](quickstart/cpp-mcp.md).

Example C++ package layout:

```text
streamfind-core-cpp-0.6.0-Windows-x86_64/
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

The native archives provide the current C++ backend and MCP interfaces. They do
not provide:

- a stable cross-version API or ABI guarantee;
- a public Python package;
- automatic installation of optional scientific tools;
- the separate R package or its Shiny application.

Optional tools such as Java and MetFrag remain explicit user-installed
components. The release packages do not download them at runtime.
