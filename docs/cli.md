# Command-line interfaces

Each native release contains four related executable surfaces. They share the
same package-relative configuration and service-discovery scope:

| Executable | Role | Communication |
| --- | --- | --- |
| `streamfind` | Public service and package CLI | Terminal commands |
| `streamfind-app` | Opens the packaged browser application | Local browser plus shared service |
| `streamfind-mcp` | MCP adapter for applications and AI agents | JSON-RPC over stdio |
| `streamfind-dev` | Development commands for external plugins and viewers | Terminal commands |

The `streamfind` CLI, app launcher, and MCP adapter use the same package-local
service. Starting one does not create a second project runtime. Keep the
package directory intact so executables can resolve `app/`, `core/`, `plugins/`,
`viewers/`, and their native libraries.

## Public CLI

Run the CLI from the extracted package directory:

```powershell
.\bin\streamfind.exe service start
.\bin\streamfind.exe service status
.\bin\streamfind.exe service list
.\bin\streamfind.exe service stop
```

On Linux, use the equivalent paths:

```sh
./bin/streamfind service start
./bin/streamfind service status
./bin/streamfind service list
./bin/streamfind service stop
```

The preferred service port is configured in `bin/streamfind.json`. Use
`service start --port <port>` to require a specific port; use a different port
for each package or worktree tested at the same time. `service status --json`
is useful for scripts and diagnostics. The service owns project sessions,
workflow execution, result publication, and the worker processes.

The service is package-scoped by the executable directory. Use
`STREAMFIND_RUNTIME_DIR` only when deliberately selecting a shared discovery
scope. Use `STREAMFIND_SERVICE_ENDPOINT` when an MCP process must attach to a
service in another scope.

## Browser app

Start the app launcher from the same package:

```powershell
.\bin\streamfind-app.exe
```

```sh
./bin/streamfind-app
```

The launcher asks the public CLI to start or reuse the package service and
opens the packaged `app/index.html` in the default browser. Closing the browser
does not stop the service. Stop it explicitly with `streamfind service stop` or
the Linux equivalent.

## MCP adapter

Configure an MCP client to launch the package executable directly:

```text
Windows: <package>\bin\streamfind-mcp.exe
Linux:   <package>/bin/streamfind-mcp
```

The adapter communicates over stdin/stdout using JSON-RPC. It attaches to or
starts the same package-scoped service used by the app and CLI. It does not
stop that service when the MCP client disconnects. See the [MCP quickstart](quickstart/cpp-mcp.md)
for initialization, discovery, project, workflow, and result requests.

## Embedded development CLI

`streamfind-dev` is included in the same release archive. It is the embedded
development framework for plugin and viewer work outside the streamfind
checkout; it is not a second runtime package or a separate release artifact.

Inspect the available commands from the extracted package:

```powershell
.\bin\streamfind-dev.exe --help
.\bin\streamfind-dev.exe doctor --json
```

The corresponding Linux executable is `./bin/streamfind-dev`. Typical native
plugin commands are:

```text
streamfind-dev plugin create <name> --workspace <workspace>
streamfind-dev plugin build --plugin <workspace> --sdk <package>
streamfind-dev plugin test --plugin <workspace> --sdk <package>
streamfind-dev plugin validate --plugin <workspace> --sdk <package>
streamfind-dev plugin package --plugin <workspace> --sdk <package>
streamfind-dev plugin install --package <package>
streamfind-dev plugin activate <plugin-id> --version <version>
```

Viewer development uses the corresponding `viewer create|build|test|validate|package|install|activate`
commands. Installed packages are kept in the user-managed extension store
under `$STREAMFIND_HOME/extensions/plugins/` and
`$STREAMFIND_HOME/extensions/viewers/`; they are selected by the package's
`enabled_plugins` and `enabled_viewers` settings.

For checkout development of streamfind itself, use [`sfdev`](../scripts/README.md)
from the repository. `sfdev` owns repository builds, tests, packaging, and
release orchestration; `streamfind-dev` owns development performed from an
extracted package.
