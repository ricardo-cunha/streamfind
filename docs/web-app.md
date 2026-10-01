# Web app

The streamfind web app is a React application that uses the native C++ service
through its HTTP and WebSocket service boundary. It does not open DuckDB files,
load native plugins, or implement domain processing in the browser.

## Release package support

The 0.4.0 native archives include the same browser application layout:

| Package | Web app contents | How to start |
| --- | --- | --- |
| Windows x86_64 | Built React assets under `app/` and the `streamfind.exe` browser launcher | Run `streamfind.exe` from the extracted package |
| Linux x86_64 | Built React assets under `app/` and the `streamfind` browser launcher | Run `streamfind` from the extracted package |

The launcher starts a package-local C++ service on an available loopback port,
waits for it to become ready, and opens the packaged `app/index.html` in the
default browser. The service serves the static assets and the backend API from
the same local origin. Close the launcher terminal or process to stop the local
service.

The native release is a preview distribution. It is not a hosted service and it
does not expose the backend beyond the local machine by default. On Linux, the
launcher uses `xdg-open`; if that command is unavailable, copy the URL printed
by the launcher into a browser manually.

## Windows release

1. Download and extract `streamfind-core-cpp-0.4.0-Windows-x86_64.zip` from the
   [GitHub release](https://github.com/ricardo-cunha/streamfind/releases/tag/v0.4.0).
2. Open the extracted package directory.
3. Run `streamfind.exe`.
4. Allow the browser to open the displayed local URL.

The executable is at the package root. The packaged static application is under
`app/`; backend executables and runtime files are under `bin/` and `core/`.
Do not move `streamfind.exe` away from the package root: it resolves the service
and vendor-runtime paths relative to the package layout.

## Linux release

1. Download and extract `streamfind-core-cpp-0.4.0-Linux-x86_64.tgz` from the
   [GitHub release](https://github.com/ricardo-cunha/streamfind/releases/tag/v0.4.0).
2. Open a terminal in the extracted package directory.
3. Run:

   ```text
   ./streamfind
   ```

4. Allow `xdg-open` to open the displayed local URL, or open that URL manually.

The executable is at the package root. The packaged static application is under
`app/`; backend executables and runtime files are under `bin/` and `core/`.
Keep the extracted directory layout intact so the launcher can find the service
and the service can find the application assets.

## Development setup

The development launcher builds the UI separately from the native release
archive. From the repository's `frontend/` directory:

```text
npm install
npm start
```

This starts the native C++ service on `http://127.0.0.1:8790`, waits for
`/session` readiness, and starts Vite on `http://127.0.0.1:5173/`. Open the Vite
URL in a browser. React and CSS changes use Vite hot module replacement; CPack
is not needed for UI-only changes.

The development launcher expects the native service at:

```text
../tmp/build/mingw-ucrt64/streamfind_service.exe
```

Build that executable from the repository root when necessary:

```text
cmake --build tmp/build/mingw-ucrt64 --target streamfind_service -j2
```

The development commands are defined in [`frontend/README.md`](https://github.com/ricardo-cunha/streamfind/blob/master/frontend/README.md).

## Frontend/backend boundary

The browser communicates with the C++ service for:

- project and session lifecycle;
- operation discovery and workflow editing;
- artifact and table access;
- progress events over WebSocket;
- visualization data and feature inspection.

The browser is therefore a client of the C++ backend, not a second backend. New
persistence, operation, reader, and plugin capabilities belong in the C++ core
or its domain plugins first.
