# streamfind frontend development

The frontend can be developed without rebuilding the release archive.

## Start the complete development environment (Windows)

From this directory:

```text
npm start
```

This starts the native C++ service on `http://127.0.0.1:8790` and the Vite UI on `http://127.0.0.1:5173/`. Open the Vite URL in a browser. React and CSS changes are applied through Vite HMR; do not run CPack for UI-only changes.

The Windows native service executable must already exist at:

```text
../tmp/build/mingw-ucrt64/streamfind_service.exe
```

Build it from the worktree root when needed:

```text
cmake --build tmp/build/mingw-ucrt64 --target streamfind_service streamfind_worker -j2
```

The Windows launcher always uses the canonical development build tree:

```text
../tmp/build/mingw-ucrt64/streamfind_service.exe
```

If a service is already healthy at `STREAMFIND_SERVICE_URL` (default `http://127.0.0.1:8790`), `npm start` reuses it and does not start another executable. It does not rebuild the C++ service automatically. After backend changes, stop the current launcher, rebuild `streamfind_service`, and run `npm start` again. There is no fallback to an older release build tree.

The launcher starts `streamfind-service.exe` and its matching `streamfind-worker.exe` from the same build tree, then starts Vite through the active Node.js executable (`node_modules/vite/bin/vite.js`) for the UI. It does not start `streamfind-mcp.exe`, the public `streamfind.exe` CLI, or the packaged root app launcher. The backend process receives MSYS2 runtime paths through `PATH` so it can load the build-tree native dependencies. Development startup fails if the service or worker is missing, preventing a stale service/worker pair from being tested accidentally.

The development launcher waits for `/session` readiness before starting Vite and stops the service when the launcher exits. Press Ctrl+C in the launcher terminal to stop both processes.

## Linux development

Start the service from the Linux build tree manually, then use
`npm run dev:ui` to start Vite. The current combined `npm start` launcher is
Windows/MSYS2-specific.

## Start only the backend (Windows)

```text
npm run dev:backend
```

Use this when running Vite separately or debugging the native service.

## Start only the UI

```text
npm run dev:ui
```

This requires a service already running at `http://127.0.0.1:8790`.

Vite uses a strict fixed port (`5173`). If that port is occupied, it fails clearly instead of silently moving the UI to another port.
