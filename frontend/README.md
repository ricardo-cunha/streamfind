# streamfind frontend

The React application and frontend plugin framework are built from this
directory. The frontend build uses TypeScript and Vite to produce static app
assets in `dist/`; the app is served by the native C++ service from a packaged
distribution.

## Build and verify changes

Run development commands from the repository root through `sfdev`:

```powershell
sfdev build frontend
sfdev test frontend
sfdev check frontend
```

These commands compile the UI, run unit tests, lint, and check formatting. They
do not start a second app server. Verify UI changes in the packaged app:

```powershell
sfdev package --frontend-only
```

This rebuilds the frontend and replaces the `app/` assets in an existing
`tmp/packaged_build/`. For native backend, worker, or packaging changes, use
`sfdev package` to rebuild and refresh the complete Windows development
package. Then launch `tmp/packaged_build/bin/streamfind-app.exe` or the
packaged CLI. The package command and manual checks are documented in
[`scripts/README.md`](../scripts/README.md).

The frontend does not own project persistence or scientific processing. It
communicates with the package's C++ service through the public HTTP and
WebSocket APIs. Frontend plugins use the shared contracts under
`src/framework/plugins/` and are bundled into the package assets.
