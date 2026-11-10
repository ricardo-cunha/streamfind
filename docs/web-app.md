# Web app

The streamfind web app is a browser interface for creating projects, designing
workflows, running operations, and inspecting results. It uses the native C++
service over HTTP and WebSocket; project files and scientific processing remain
on the local machine.

## Using the app

The project hub lists the workflow projects available to the local service.
Create a project, open an existing DuckDB project, or start from a workflow
demo. Opening a project takes you to its workflow canvas.

On the canvas, add operation nodes, connect compatible input and output ports,
and configure each node's parameters. Run an operation to inspect one step, or
run the workflow to execute connected steps in dependency order. The activity
log shows operation progress and failures. Published results can be opened
from output ports and explored with the result renderer provided for their
type, including tables and domain-specific visualizations.

The app and MCP clients use the same local service. A project opened through
the service, and results produced by either client, are available to the other
client through the shared project state.

## Release package support

The {{ streamfind_version }} native archives include the same browser application layout:

| Package | Web app contents | How to start |
| --- | --- | --- |
| Windows x86_64 | Built React assets under `app/` and executables under `bin/` | Run `bin\streamfind-app.exe` or use `bin\streamfind.exe` from a terminal |
| Linux x86_64 | Built React assets under `app/` and executables under `bin/` | Run `./bin/streamfind-app` or use `./bin/streamfind` from a terminal |

The application launcher asks the public CLI to start or reuse the package-local
tray manager. The manager owns and supervises `streamfind-server`, then the
launcher opens the packaged `app/index.html` in the default browser. The server
serves the static assets and backend API from the same local origin. Closing the
browser or launcher leaves the manager and server running. Closing the Windows
tray manager stops the server; `streamfind service stop` stops both. On Linux,
the same manager runs headlessly because desktop tray support is not included.

The application is local rather than hosted and does not expose the backend
beyond the local machine by default. On Linux, the
launcher uses `xdg-open`; if that command is unavailable, copy the URL printed
by the launcher into a browser manually.

## Windows release

1. Download and extract `streamfind-core-cpp-{{ streamfind_version }}-Windows-x86_64.zip` from the
   release assets.
2. Open the extracted package directory.
3. Run `bin\streamfind-app.exe`.
4. Allow the browser to open the displayed local URL.

The application launcher and public CLI are both under `bin/`. The packaged
static application is under `app/`; backend executables and runtime
files are under `bin/` and `core/`. Keep the package layout intact.

From a terminal, use:

```powershell
.\bin\streamfind.exe service start
.\bin\streamfind.exe service list
.\bin\streamfind.exe service status --json
.\bin\streamfind.exe service stop
```

The packaged service prefers loopback port `8787`. If another application is
using it, streamfind automatically selects an available port in the next 128
ports and reports the selected endpoint in `service list`. The app launcher
opens the endpoint published by the running service. To require a particular
port, use `streamfind service start --port <port>`; this explicit choice fails
with a clear error if the port is unavailable. The behavior can be configured
with `server.automatic_port_fallback` in `bin/streamfind.json`.

The MCP executable starts or attaches to this same manager when it starts. It
does not stop the shared server when the MCP client disconnects or exits. The
app and MCP therefore share projects and active runs through one server.

## Linux release

1. Download and extract `streamfind-core-cpp-{{ streamfind_version }}-Linux-x86_64.tgz` from the
   release assets.
2. Open a terminal in the extracted package directory.
3. Run:

   ```text
   ./bin/streamfind-app
   ```

4. Allow `xdg-open` to open the displayed local URL, or open that URL manually.

The application launcher and public CLI are both under `bin/`. The packaged
static application is under `app/`; backend executables and runtime
files are under `bin/` and `core/`. Keep the extracted directory layout intact.

From a terminal, use `./bin/streamfind service start`, `./bin/streamfind service list`,
`./bin/streamfind service status`, and `./bin/streamfind service stop`. `service start`
launches the headless manager and server; `service stop` stops both. When more than one service is
running, provide `--instance <id>` to select the service explicitly.

## Frontend/backend boundary

The browser communicates with the C++ service for:

- project and session lifecycle;
- operation discovery and workflow editing;
- result and table access;
- progress events over WebSocket;
- visualization data and feature inspection.

The browser is therefore a client of the C++ backend, not a second backend. New
persistence, operation, reader, and plugin capabilities belong in the C++ core
or its domain plugins first.
