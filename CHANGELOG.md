# Changelog

## 0.2.1 — 2026-09-12

### Fixed

- Replaced automatic log property refreshes with a **Refresh log** button to
  avoid rebuilding the properties UI while its controls are active.
- Added regression coverage for repeated log refreshes in nested settings groups.
- Added a warning when OBS's browser source is unavailable and the overlay
  cannot render.
- Updated build and bundle version metadata to match the release version.

## 0.2.0 — 2026-09-12

### Added

- **Line Gap** slider under Line Background (0–1000 px, default 0), adding
  bottom margin to each line except the last child.

- **Hide when empty** under Line Background, using the overlay’s `empty-line`
  class to hide blank and whitespace-only lines when enabled.

- Native settings tabs on OBS builds with the plugin property tabs feature,
  with stacked-section fallback on older OBS versions. Tab selection survives
  property refreshes without changing source settings.

- A Stage Display instance list and rescan button using Bonjour discovery for
  ProPresenter 7 services on the same computer and local network.
- Saved service selection that resolves the instance's current hostname and
  port when opening or reconnecting, including after an address change.
- Standalone discovery and connection responsiveness regression tests.

### Changed

- Moved the log into the bottom of Connection and replaced the static preview
  with a read-only live view of in-memory messages. New messages refresh the
  view at most twice per second; log-file access remains available.

### Fixed

- OBS could stall when changing connection settings while the previous attempt
  was resolving, connecting, or waiting for data. Settings changes now queue on
  a persistent background worker instead of joining the previous connection.
- Network socket waits are now cancellable, with timeouts for connection
  attempts, WebSocket handshakes, and HTTP responses. Bonjour service and
  hostname lookups also support cancellation.
- Connection sockets are owned and closed by the network worker, avoiding
  cross-thread socket closure during reconnects.

### Compatibility and upgrading

- Manual hostname/IP and port entry remains available. Windows discovery
  requires Bonjour; macOS includes it.
- Offline selections retry the same service and retain the last received text.
  Renamed services must be selected again.
- Restart OBS after replacing the plugin to load the fix.
- Validated on Apple Silicon, including user confirmation in OBS. Windows
  validation remains outstanding.
