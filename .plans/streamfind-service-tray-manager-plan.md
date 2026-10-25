# StreamFind Service Tray Manager Plan

## Status

Implementation in progress. The shared service status client, configuration,
Windows tray executable, CLI launch command, package installation, and service
host startup integration are implemented. Linux currently builds a headless
status fallback because the supported WSL image has no StatusNotifier/AppIndicator
development runtime. The plan assumes the
service remains a long-lived local process shared by the browser application,
MCP, and the `streamfind` terminal command.

## Purpose

Make the running StreamFind service visible and manageable after the application
window or browser is closed. The user should be able to see whether the service
is active, identify its endpoint and process, open the application, inspect
runtime activity, and stop or restart the service without opening a terminal.

The tray manager must be a client of the service. It must not open DuckDB,
manage workers directly, or create a second service instance.

## User-facing result

When the service is running, the operating-system status area shows a StreamFind
icon with a state-specific appearance:

- starting: muted or animated icon;
- ready: normal StreamFind icon;
- busy: normal icon with a small activity marker;
- warning: attention marker when the service reports an error;
- stopped or disconnected: unavailable state or no resident icon.

The tray menu should show, without requiring a browser:

- service state;
- instance ID;
- endpoint and configured port;
- service process ID;
- attached project, if any;
- active workflow/operation count;
- active worker count;
- last error, if present.

The menu should provide:

- Open StreamFind;
- Open service status;
- Copy endpoint;
- Open activity log;
- Stop service;
- Restart service;
- Exit tray manager.

Stopping the tray manager must not stop the service. Stopping the service must
be an explicit menu action and must use the same authenticated service command
as the CLI and MCP.

## Architecture

```text
streamfind-app.exe / streamfind-app
              |
       tray manager client
              |
      service discovery + HTTP API
              |
      streamfind-service process
              |
       supervised workers
```

The tray manager is a small platform UI process with shared service-client
logic. It discovers an already-running service, attaches to it, polls or
subscribes to status changes, and renders native menu and notification events.
It must never launch a second service when a healthy instance is already
available.

The app host may launch the tray manager when it starts the service. The CLI
must also be able to start the tray manager independently. MCP remains a
headless client and does not require the tray manager.

## Service discovery and ownership

Use the existing local discovery record as the source of truth. Extend it only
where necessary with:

- protocol version;
- service instance ID;
- endpoint and bound address;
- service PID;
- configured project/session information;
- authenticated local-client token or protected token reference;
- startup time and last heartbeat;
- service state.

Discovery records must be written atomically and removed on clean shutdown.
Stale records must be rejected by a health check and never used to control an
unrelated process. The tray manager should display a clear disconnected state
when discovery exists but the health check fails.

The tray manager should prefer the service instance started by the current
StreamFind installation, but it must show the discovered instance ID and
endpoint so the user can understand which service is being managed.

## Shared service API

Add or reuse service endpoints for:

- health and identity;
- runtime summary;
- active project/session summary;
- recent activity events;
- service stop;
- service restart request, if supported by the host;
- log retrieval or a log-file location;
- browser/app launch URL.

All mutating commands must include an idempotency key and the discovered
instance ID. The service must reject a command addressed to another instance.
The tray manager must use the same HTTP client and response models as the app
and MCP, rather than implementing a private protocol.

For live updates, prefer the existing event stream/WebSocket mechanism. The
tray manager should reconnect with an event cursor and fall back to bounded
polling when the stream is unavailable. It should not poll more frequently than
the configured service-status interval.

## Windows implementation

Implement a native notification-area client using the Win32 Shell_NotifyIcon
API. Keep the service process independent from the tray process so closing the
browser or app window does not terminate the service.

Required Windows behavior:

- create the icon only after service discovery succeeds;
- show a balloon/toast only for meaningful transitions or failures;
- use the StreamFind base icon and the app/mcp branding rules consistently;
- support high-DPI icon sizes and light/dark Windows themes where available;
- handle Explorer restart by re-registering the icon;
- close menus and release the icon cleanly on exit;
- avoid a hidden console window for normal packaged use.

The tray manager should be a separate packaged executable, for example
`streamfind-tray.exe`, placed in `bin`. It may share the service-client library
with the app host, CLI, and MCP.

## Linux implementation

Implement the tray client through the desktop's StatusNotifierItem/AppIndicator
interface. Do not assume every Linux desktop has a traditional system tray.

Required Linux behavior:

- use the desktop integration available at runtime;
- remain usable on KDE Plasma and Ubuntu desktops with AppIndicator support;
- show a clear message when the desktop has no tray host;
- keep all service controls available through the CLI and web status page;
- support launching as a user process without root privileges;
- provide an optional systemd user-service integration for startup and restart.

The Linux executable should be named `streamfind-tray` and be installed in
`bin`. A missing tray integration must not prevent the service, app, or MCP from
running.

## CLI integration

Extend the existing `streamfind` command, keeping the public command name
unchanged:

```text
streamfind service list
streamfind service status
streamfind service start
streamfind service stop
streamfind service restart
streamfind service tray
```

`service stop` without an explicit instance should stop the uniquely discovered
healthy local service. If multiple services are found, it must refuse and list
the available instance IDs instead of guessing.

`service tray` should start or foreground the tray manager and attach it to the
discovered service. The command must print the selected instance and endpoint.

## Configuration

Use the existing package-relative `streamfind.json` configuration. Add a
`tray` section only for user-visible policy, for example:

```json
{
  "tray": {
    "enabled": true,
    "start_with_app": true,
    "start_with_service": true,
    "status_interval_ms": 2000,
    "notifications": true
  }
}
```

The service endpoint, host, port, and service identity remain owned by the
`server` configuration and discovery record. The tray must not invent a second
port or silently override the service configuration.

## Packaging and layout

Keep one consistent runtime layout on Windows and Linux:

```text
streamfind-<version>/
├── bin/
│   ├── streamfind
│   ├── streamfind-app
│   ├── streamfind-mcp
│   ├── streamfind-service
│   ├── streamfind-worker
│   ├── streamfind-tray
│   ├── streamfind.json
│   └── native runtime libraries
├── app/
├── core/
├── plugins/
└── licenses/
```

Windows adds the `.exe` suffix. The tray executable must resolve all native
dependencies from `bin`, and Linux binaries must use the same package-relative
RPATH convention as the service and plugins. No runtime DLL/SO files should be
copied into a `core/vendors` directory or beside plugin libraries.

The release validator must check:

- tray executable is present;
- tray executable starts and attaches to a test service;
- package contains no legacy runtime vendor directory;
- native dependency closure resolves from `bin` and plugin packages;
- service, app, MCP, CLI, and tray use the same configuration file.

## Security and safety

- Bind the service to loopback by default.
- Keep service control endpoints local-only.
- Require the discovery token or equivalent local authentication for stop and
  restart commands.
- Never expose database paths or worker command lines unnecessarily in desktop
  notifications.
- Confirm before stopping a service with active workflows, or present the
  active-operation warning and require a second explicit action.
- Use idempotent stop requests so repeated clicks are safe.
- Do not terminate the service when the tray process exits.

## Implementation phases

### Phase 1: shared status model

1. Define service identity, health, runtime summary, and event response models.
2. Reuse the existing app/MCP service client and discovery code.
3. Add tests for stale records, instance mismatch, reconnect, and idempotent
   stop requests.

### Phase 2: service endpoints and event stream

1. Add the status summary and recent activity API if missing.
2. Expose the service log/event cursor to local clients.
3. Add stop/restart authorization and active-workflow warnings.
4. Verify the app and MCP still observe identical state and event ordering.

### Phase 3: Windows tray manager

1. Add `streamfind-tray.exe` with native notification-area integration.
2. Implement menu state, endpoint copying, app launch, log access, and stop.
3. Add Explorer restart and high-DPI handling.
4. Start it from the app when configured, without coupling its lifetime to the
   browser window.

### Phase 4: Linux tray manager

1. Add `streamfind-tray` with StatusNotifierItem/AppIndicator support.
2. Implement the same menu and service-client behavior.
3. Add graceful no-tray-host behavior and CLI fallback messaging.
4. Add optional systemd user-service documentation and packaging tests.

### Phase 5: packaging and release integration

1. Install the tray executable into `bin` for both platforms.
2. Add tray branding assets and licenses where required.
3. Update package manifests, release validators, and documentation.
4. Verify package-relative resolution from a directory containing spaces and
   from a different current working directory.

### Phase 6: end-to-end validation

1. Start the service, launch the tray manager, and verify the ready indicator.
2. Open a project from the app and verify the tray shows the same project.
3. Start a workflow from the app and inspect it through the tray status view.
4. Close the browser/app window and verify the service and workflow continue.
5. Attach MCP and verify it sees the same instance and runtime state.
6. Stop the service from the tray and verify the app/MCP receive the shutdown
   event and no stale discovery record remains.
7. Repeat on Windows and WSL/Linux, including a desktop without tray support.

## Acceptance criteria

- The user can always determine whether a StreamFind service is running.
- The tray never starts a duplicate service or accesses DuckDB directly.
- App, MCP, CLI, and tray report the same service identity and runtime state.
- Closing the browser or app does not stop an active service or workflow.
- Explicit stop works from the tray and `streamfind service stop`.
- Windows and Linux packages contain the tray executable and resolve all native
  dependencies from the centralized `bin` layout.
- Headless Linux environments remain fully usable through CLI and HTTP APIs.
