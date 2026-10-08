# Releases

The current distribution is a versioned **preview release** of the native C++
platform. Version 0.5.0 provides self-contained C++ runtime packages for
Windows x64 and Linux x86_64, plus optional developer-kit packages.

The GitHub Release is the authoritative distribution location.

The project source is maintained at
<https://github.com/ricardo-cunha/streamfind>.

## Project version: {{ streamfind_version }}

The native C++ project version is **{{ streamfind_version }}**.

## 0.5.0 test release scope

Version 0.5.0 contains:

- Windows x86_64 ZIP;
- Linux x86_64 TGZ;
- an optional C++ plugin developer-kit archive;
- a checksum manifest covering the published C++ archives.

The archives are intended for testing on other machines. They do not provide a
stable cross-version C++ ABI.

| Package | Archive | Platform | SHA-256 |
| --- | --- | --- | --- |
| C++ core | `streamfind-core-cpp-0.5.0-Windows-x86_64.zip` | Windows x64 | See `sha256sums.txt` |
| C++ core | `streamfind-core-cpp-0.5.0-Linux-x86_64.tgz` | Linux x86_64 | See `sha256sums.txt` |
| Plugin developer kit | `streamfind-plugin-dev-0.5.0-Windows-x86_64.zip` | Windows x64 | See `sha256sums.txt` |
| Plugin developer kit | `streamfind-plugin-dev-0.5.0-Linux-x86_64.tar.gz` | Linux x86_64 | See `sha256sums.txt` |

The complete checksum list is distributed as `sha256sums.txt` alongside the
archives.

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
see the [C++ MCP quickstart](quickstart/cpp-mcp.md).

Example C++ package layout:

```text
streamfind-core-cpp-0.5.0-Windows-x86_64/
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
