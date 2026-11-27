# Develop plugins with a release package

The Windows x64 and Linux x86_64 release archives include the **streamfind
development framework**. Use the matching archive's `streamfind-dev` command to
create, build, validate, package, install, and activate plugins and viewers.
This workflow uses the tools and SDK shipped for that runtime version; it does
not require a source checkout. Follow this guide and the commands reported by
the extracted package's `bin/streamfind-dev`.

## 1. Extract a matching release

Download and extract the Windows or Linux archive from [Releases](releases.md).
Keep the extracted package intact: `streamfind-dev` resolves its provisioning
helper and SDK relative to that package. Use that package to run the service
that will load the extension.

```powershell
# Windows, from the extracted package
bin\streamfind-dev.exe --version
bin\streamfind-dev.exe doctor --json
bin\streamfind-dev.exe list --target all
```

```sh
# Linux, from the extracted package
./bin/streamfind-dev --version
./bin/streamfind-dev doctor --json
./bin/streamfind-dev list --target all
```

The reported product version should match the runtime archive. The
provisioning protocol version is reported separately. `doctor` shows the
resolved workspace, managed tool directory, installed SDK/package state, and
extension registry; `list` reports installed plugins and viewers. The default
source workspace is `Documents/streamfind-dev` and the managed home is
`~/.streamfind` (Windows: `%USERPROFILE%\.streamfind`). Override these with
`--workspace`, `--managed-home`, or `STREAMFIND_HOME`.

## 2. Provision development tools

Provision only the toolchain needed for your extension:

```text
streamfind-dev setup --target native
streamfind-dev setup --target viewer
streamfind-dev setup --target python --workspace <workspace>
streamfind-dev setup --target all --workspace <workspace>
```

The native target installs the platform's C++ build prerequisites (MSYS2
UCRT64 on Windows, native packages on supported Linux distributions). The
viewer target installs the pinned Node.js and npm toolchain. The Python target
installs the pinned Python runtime and creates a workspace-local `.venv`; use
`--requirements <file>` only when you want that requirements file installed.
`--offline` uses locally available verified tool archives, and `--dry-run`
prints the planned setup without applying it. Setup is managed by the
release-bundled provisioner, not by the repository's development scripts.

## 3. Build a native C++ plugin

Create a plugin workspace with the bundled scaffold, then add operations as
needed:

```text
streamfind-dev plugin create <name> --workspace <workspace>
streamfind-dev plugin add-operation <operation-name> --plugin <plugin-path>
```

The scaffold includes a native CMake project, plugin manifest, semantic
catalogue, and starter operation. Edit these sources using the generated
plugin's README and the SDK contract. Plugin IDs and operation IDs are part of
the package interface; keep them stable across releases of your plugin.

Build and check the plugin against the SDK from the **same extracted runtime
archive**:

```text
streamfind-dev plugin build --plugin <plugin-path> --sdk <package-root>
streamfind-dev plugin test --plugin <plugin-path> --sdk <package-root>
streamfind-dev plugin validate --plugin <plugin-path> --sdk <package-root>
streamfind-dev plugin package --plugin <plugin-path> --sdk <package-root>
```

`package` builds and validates before writing a platform-specific package. You
can inspect an existing package with:

```text
streamfind-dev inspect --package <package-path> --target native
```

Use the `--build` option to select a build directory when needed. The plugin
commands resolve compilers and CMake from the provisioned native toolchain.

## 4. Build a result viewer

Create a viewer for a fully qualified result contract IRI from the runtime's
semantic catalogue:

```text
streamfind-dev viewer create <name> --contract <https-result-contract-IRI> --workspace <workspace>
streamfind-dev setup --target viewer
streamfind-dev viewer build --viewer <viewer-path>
streamfind-dev viewer test --viewer <viewer-path>
streamfind-dev viewer validate --viewer <viewer-path>
streamfind-dev viewer package --viewer <viewer-path>
```

The scaffold uses the TypeScript/React viewer API and the matching viewer SDK
contract. Build and test use the package-managed Node toolchain. Viewer
validation checks the viewer manifest, result contract, and built bundle;
packaging writes a checksummed viewer package.

## 5. Install and activate an extension

Install a validated package into the managed extension store, then activate its
manifest ID and version:

```text
streamfind-dev plugin install --package <plugin-package>
streamfind-dev plugin activate <plugin-id> --version <version>

streamfind-dev viewer install --package <viewer-package>
streamfind-dev viewer activate <viewer-id> --version <version>
```

Install and activation are separate so an extension is not loaded until you
select it. The registry records installed versions, source provenance, and
compatibility status. `doctor --json` reports the registry and compatibility
findings; `list --target native|viewer|all` shows installed entries. Use
`deactivate` to stop selecting a version, or `uninstall` to remove it from the
registry and managed extension store.

Configure the target streamfind service to use the same extension registry and
managed deployment roots, then enable the IDs in `streamfind.json`. For example:

```json
{
  "plugin_roots": {
    "deployment": ["C:/Users/<user>/.streamfind/extensions/plugins"]
  },
  "viewer_roots": {
    "deployment": ["C:/Users/<user>/.streamfind/extensions/viewers"]
  },
  "registry_path": "C:/Users/<user>/.streamfind/extensions/registry.json",
  "enabled_plugins": ["custom.plugin.example"],
  "enabled_viewers": ["@streamfind/example-viewer"]
}
```

With `registry_path` set, the service resolves active package versions through
the shared registry; deployment roots are used for package lookup when no
registry is configured. The service loads only extension IDs selected in its
enabled lists. Restart or reload the target service after changing its
configuration. The extension store is separate from the release directory and
the source workspace.

## 6. Maintain extensions across runtime updates

Use `streamfind-dev doctor --json` to review installed versions and runtime
compatibility. Native plugins may need to be rebuilt against the new runtime's
SDK. When retained source is available, rebuild and revalidate it against the
matching archive before activating it:

```text
streamfind-dev rebuild --plugin <plugin-source> --sdk <new-package-root>
```

The runtime does not promise a stable cross-version C++ ABI. Review viewer API
and result-contract compatibility as well, then test the selected extension
with the target package. Keep the previous package and extension source until
the updated extension has been checked.

## Command reference

The embedded CLI is the authoritative command reference for the release being
used. Run `streamfind-dev` without arguments to print usage. The main commands
are:

| Command | Purpose |
| --- | --- |
| `doctor` | Inspect workspace, toolchain, registry, and compatibility |
| `setup` | Provision native, viewer, or Python development tools |
| `list` / `inspect` | Review installed extensions or a package |
| `plugin create|add-operation|build|test|validate|package` | Develop native plugins |
| `plugin install|activate|deactivate|uninstall` | Manage native plugin packages |
| `viewer create|build|test|validate|package` | Develop result viewers |
| `viewer install|activate|deactivate|uninstall` | Manage viewer packages |
| `rebuild` | Rebuild a retained plugin source against a selected SDK |

See [Command-line interfaces](cli.md) for how `streamfind-dev` relates to the
runtime's `streamfind`, `streamfind-app`, and `streamfind-mcp` executables.
