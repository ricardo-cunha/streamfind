# streamfind frontend development

The frontend can be developed without rebuilding the release archive.

## Start the complete development environment

From this directory:

```text
npm start
```

This starts the native C++ service on `http://127.0.0.1:8790` and the Vite UI on `http://127.0.0.1:5173/`. Open the Vite URL in a browser. React and CSS changes are applied through Vite HMR; do not run CPack for UI-only changes.

The native service executable must already exist at:

```text
../tmp/build/mingw-ucrt64/streamfind_service.exe
```

Build it from the worktree root when needed:

```text
cmake --build tmp/build/mingw-ucrt64 --target streamfind_service -j2
```

The launcher always uses the canonical development build tree:

```text
../tmp/build/mingw-ucrt64/streamfind_service.exe
```

If a service is already healthy at `STREAMFIND_SERVICE_URL` (default `http://127.0.0.1:8790`), `npm start` reuses it and does not start another executable. It does not rebuild the C++ service automatically. After backend changes, stop the current launcher, rebuild `streamfind_service`, and run `npm start` again. There is no fallback to an older release build tree.

The launcher starts `streamfind_service.exe` for the backend and starts Vite through the active Node.js executable (`node_modules/vite/bin/vite.js`) for the UI. It does not start `streamfind_mcp.exe`, `streamfind_cli.exe`, or the packaged root `streamfind.exe`. The backend process receives MSYS2 runtime paths through `PATH` so it can load the build-tree native dependencies.

The development launcher waits for `/session` readiness before starting Vite and stops the service when the launcher exits. Press Ctrl+C in the launcher terminal to stop both processes.

## Start only the backend

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
