# streamfind frontend development

The frontend can be developed without rebuilding the release archive.

## Start the complete development environment

From this directory:

```text
npm start
```

This starts the native C++ service on `http://127.0.0.1:8790` and the Vite UI on `http://127.0.0.1:5173/`. Open the Vite URL in a browser. React and CSS changes are applied through Vite HMR; do not run CPack for UI-only changes.

The native service and matching worker executables must already exist in the
platform build tree:

```text
Windows: ../tmp/build/windows/core/streamfind-server.exe
Linux:   ../tmp/build/linux/core/streamfind-server
```

The launcher selects the canonical development build tree for the current
platform. Configure and build it from the worktree root when needed:

```text
sfdev configure
sfdev build backend
```

If a service is already healthy at `STREAMFIND_SERVICE_URL` (default `http://127.0.0.1:8790`), `npm start` reuses it and does not start another executable. It does not rebuild the C++ service automatically. After backend changes, stop the service explicitly with `streamfind service stop`, rebuild `streamfind_service` and `streamfind_worker`, then run `npm start` again. There is no fallback to an older release build tree.

The launcher starts `streamfind-server` and its matching `streamfind-worker` from the same build tree, then starts Vite through the active Node.js executable (`node_modules/vite/bin/vite.js`) for the UI. It does not start `streamfind-mcp`, the public `streamfind` CLI, or the packaged app launcher. On Windows, the backend process receives MSYS2 runtime paths through `PATH` so it can load the build-tree native dependencies. Development startup fails if the service or worker is missing, preventing a stale service/worker pair from being tested accidentally.

The development launcher waits for `/session` readiness before starting Vite. If it started the service, Ctrl+C stops both the service and Vite. If it reused a service that was already ready, that service remains running when the launcher exits.

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
