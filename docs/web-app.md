# Web app

The streamfind web app is a browser interface for creating projects, designing
workflows, running operations, and inspecting results. It uses the native C++
service over HTTP and WebSocket; project files and scientific processing remain
on the local machine.

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

## Local development

From the repository's `frontend/` directory on Windows:

```text
npm install
npm start
```

This starts the native C++ service on `http://127.0.0.1:8790` and Vite on
`http://127.0.0.1:5173/`. Open the Vite URL in a browser. React and CSS changes
use Vite hot module replacement.

The checked-in development launcher currently expects the Windows native
service at:

```text
../tmp/build/windows/core/streamfind-server.exe
```

Build that executable from the repository root when necessary:

```text
cmake --build tmp/build/windows/core --target streamfind_service -j2
```

On Linux, the checked-in `npm start` launcher is not yet platform-neutral: it
currently resolves the Windows UCRT64 executable. Start the Linux service
manually from the Linux build tree instead:

```text
../tmp/build/linux/core/streamfind-server
```

Build it from the repository root when necessary:

```bash
cmake --build tmp/build/linux/core --target streamfind_service --parallel
```

Then, from `frontend/`, run `npm run dev:ui`. The Vite URL and service URL are
`http://127.0.0.1:5173/` and `http://127.0.0.1:8790` on both platforms.

The development commands are defined in [`frontend/README.md`](https://github.com/ricardo-cunha/streamfind/blob/master/frontend/README.md).

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
