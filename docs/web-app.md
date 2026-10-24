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
C++ service on loopback, waits for it to become ready, and opens the packaged
`app/index.html` in the default browser. The service serves the static assets and
backend API from the same local origin. Closing the browser or launcher does not
stop the service; stop it explicitly with `bin\\streamfind service stop`.

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
`./bin/streamfind service status`, and `./bin/streamfind service stop`. When more than one service is
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
../tmp/build/mingw-ucrt64/streamfind-service.exe
```

Build that executable from the repository root when necessary:

```text
cmake --build tmp/build/mingw-ucrt64 --target streamfind_service -j2
```

On Linux, the checked-in `npm start` launcher is not yet platform-neutral: it
currently resolves the Windows UCRT64 executable. Start the Linux service
manually from the Linux build tree instead:

```text
../tmp/build/linux-gcc/streamfind-service
```

Build it from the repository root when necessary:

```bash
cmake --build tmp/build/linux-gcc --target streamfind_service --parallel
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
