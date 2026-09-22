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
