# OBS ProPresenter Lyrics

Native OBS source plugin for displaying live ProPresenter lyrics from the
ProPresenter Stage Display API.

This is no longer an OBS Python script. Users install a normal OBS plugin
package for their platform, then add a `ProPresenter Lyrics` source in OBS.

## User Install Targets

The project is set up to produce these release artifacts:

- `propresenter-lyrics-macos-arm64.zip` for Apple Silicon Macs.
- `propresenter-lyrics-macos-x86_64.zip` for Intel Macs.
- `propresenter-lyrics-windows-x64.zip` for 64-bit Windows.

## What The Plugin Does

- Adds a native OBS source named `ProPresenter Lyrics`.
- Discovers ProPresenter 7 Stage Display services on this computer and the local
  network using Bonjour, with a selectable list and a rescan button.
- Remembers the selected service identity and resolves its current host and port
  when the source opens or reconnects.
- Resolves and connects in the background, keeping OBS responsive while a
  display is offline or reconnecting.
- Connects to ProPresenter at `ws://<host>:<port>/stagedisplay`.
- Authenticates with the configured ProPresenter password.
- Requests Stage Display frame values for current slide text, next slide text,
  notes, or stage message.
- Uses `/v1/status/slide` as an optional HTTP fallback.
- Renders the lyric overlay internally through OBS's built-in Browser Source.
- Supports presets and controls for color, opacity, font family, font size,
  font weight, italic, letter spacing, alignment, line height, max lines,
  shrink-to-fit text sizing, text shadow, line background, and crossfade
  duration.

## Connect a Stage Display

1. In ProPresenter, enable network control and Stage Display API access, and
   configure a Stage Display password.
2. Add a **ProPresenter Lyrics** source in OBS and open its properties.
3. In **Connection**, choose a **Stage Display instance** from the discovered
   list. Discovery starts when the source is created and includes instances
   running on the same computer. Use **Scan for Stage Displays** to refresh it.
4. Enter the Stage Display password. Discovery supplies the host and port;
   you do not need to enter them for a discovered instance.
5. Choose the text channel to display and adjust the overlay's appearance.

### Remembered connections

The selected service identity is saved with the OBS source. When the source
opens or reconnects, the plugin resolves that identity to its current host and
port, so an IP address or port change does not require selecting it again.

If the selected instance is offline, the plugin retries it in the background.
It does not automatically switch to another discovered instance. An unavailable
selection remains in the list with **not found (will retry)**. If you rename the
service in ProPresenter, scan again and select its new name. Service names are
discovery identities, not authenticated hardware identifiers.

### Manual connections and discovery requirements

Select **Manual host and port** to enter a hostname or IP address and port
explicitly. The manual port defaults to `50001`; use the port configured in
ProPresenter if it differs. Discovered instances use their advertised port,
which may differ from this default.

Discovery uses ProPresenter 7's `_pro7stagedsply._tcp` Bonjour advertisement.
Bonjour is built into macOS. Windows requires Bonjour's `dnssd.dll` and its
service; manual connections remain available without it. Discovery only lists
services visible through local multicast DNS. It does not scan every IP address
or port, and separate subnets, isolated Wi-Fi, or firewalls can prevent discovery.

### Offline and reconnecting behavior

Resolving, connecting, and reading from ProPresenter happen on a background
worker. Changing connection settings queues the new connection without making
OBS wait for the previous attempt to finish. Socket waits are cancellable;
connection attempts, WebSocket handshakes, and HTTP responses have timeouts.
OBS can continue rendering other sources while ProPresenter is unavailable.

A disconnect does not clear the last received lyrics. The overlay retains its
last text until a subsequent update changes it; hide the source in OBS if you
need to remove stale lyrics while the connection is unavailable.

## Settings tabs

On the custom OBS build with plugin property tabs, source properties use native
OBS tabs: **Connection**, **Size**, **Presets**, **Typography**, **Shadow**,
**Line Background**, **Transitions**, and **Custom CSS**.

A read-only **Live connection log** sits at the bottom of **Connection**. It
shows the latest 14 messages from the source’s in-memory log and refreshes at
most twice per second when new messages arrive, including while disconnected.
It does not read or tail the log file. **Open log file** and **Open log folder**
remain available for longer history. Live messages are display-only and are
not written into source settings.

The current OBS properties API refreshes the properties view to update the log;
updates are batched, and unchanged logs do not trigger refreshes.

OBS preserves the selected tab while the properties view refreshes, including
when a discovery scan completes. Switching tabs does not change source settings
or reconnect the client. The selected tab is view state; reopening the properties
window does not promise to restore the previously selected tab.

On OBS versions without the tabs feature, the same controls appear as stacked,
labeled sections. Existing source settings and preset values remain compatible.
The plugin continues to build against the baseline OBS SDK, using the documented
`OBS_GROUP_TAB` enum value when the SDK does not yet declare it.

Under **Line Background**, enable **Hide when empty** to remove blank or
whitespace-only lines, including their background and spacing. The overlay
marks those lines with the `empty-line` class; they use `display: none` while
this option is checked. It defaults to off and is saved with style presets.

**Line Gap** in the same section adds a bottom margin to each line except the
last child. Its range is **0–1000 px**, defaulting to **0 px**, and it is saved
with style presets. Hidden empty lines do not occupy margin space.

## Troubleshooting connections

| Symptom | What to check |
| --- | --- |
| No instances appear | Confirm Stage Display access is enabled in ProPresenter, then scan again. Check Bonjour availability and local network visibility, or use a manual connection. |
| Saved instance shows **not found (will retry)** | Start that ProPresenter instance. If its service name changed, scan and select the new name. |
| Instance appears but lyrics do not update | Verify the Stage Display password and selected text channel. Discovery finds the service but does not authenticate it. Check **Live connection log** at the bottom of **Connection** for details. |
| OBS still pauses while reconnecting after an upgrade | Confirm the updated plugin is installed and restart OBS to load it. |

## Build

You need OBS development files available to CMake through `libobsConfig.cmake`.
The official OBS plugin template documents the expected Windows and macOS build
flow.

### Apple Silicon

The OBS source checkout must match the OBS app version you will load the plugin
into. If OBS reports `compiled with newer libobs`, build the tag matching your
installed OBS version.

First build and install a local OBS SDK:

```sh
cd "/Users/reginald/Documents/OBS ProPresenter Lyrics Plugin"
bash scripts/rebuild-obs-sdk-for-installed-obs.sh
```

Then build this plugin:

```sh
bash scripts/build-macos-arm64.sh
```

The plugin CMake project now auto-detects these common `libobsConfig.cmake`
locations:

- `LIBOBS_DIR` or `LIBOBS_CONFIG` environment variables.
- `~/Developer/obs-sdk-*`
- `~/Developer/obs-sdk`
- `~/Developer/obs-studio/build_macos/libobs`
- `../obs-sdk`
- `../obs-studio/build_macos/libobs`

You can still locate the OBS CMake package manually:

```sh
find "$HOME/Developer" \( -name 'libobsConfig.cmake' -o -name 'libobs-config.cmake' \) -print
```

If that command prints a path like:

```text
/Users/you/Developer/obs-sdk/lib/cmake/libobs/libobsConfig.cmake
```

you can pass the containing directory to CMake:

```sh
cmake --preset macos-arm64 --fresh \
  -Dlibobs_DIR="$HOME/Developer/obs-sdk/lib/cmake/libobs"
```

If CMake says `Xcode 1.5 not supported`, install full Xcode and point command
line tools at it:

```sh
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
sudo xcodebuild -license accept
xcodebuild -runFirstLaunch
xcodebuild -version
```

If CMake finds `libobs` but then reports missing `SIMDe`, Homebrew may have
installed SIMDe headers without a CMake config. This repo includes a shim at
`cmake/shims/SIMDe`, and `scripts/build-macos-arm64.sh` passes it automatically.

Manual full build command:

```sh
cmake --preset macos-arm64 --fresh \
  -Dlibobs_DIR="$HOME/Developer/obs-sdk-32.1.2/lib/cmake/libobs" \
  -DSIMDe_DIR="$PWD/cmake/shims/SIMDe"

cmake --build --preset macos-arm64

cmake --install build/macos-arm64 \
  --prefix dist/macos-arm64
```

Other presets:

```sh
cmake --preset macos-x86_64
cmake --preset windows-x64
```

## Install Layout

Windows package layout:

```text
propresenter-lyrics/
  bin/64bit/propresenter-lyrics.dll
  data/locale/en-US.ini
  data/icon/plugin-icon.svg
```

macOS package layout:

```text
propresenter-lyrics.plugin
```

Copy the Windows plugin folder into:

```text
C:\ProgramData\obs-studio\plugins
```

Copy the macOS `.plugin` bundle into:

```text
~/Library/Application Support/obs-studio/plugins
```

Restart OBS after installing or replacing the plugin to load the new build.

## Notes

- OBS's Browser Source plugin must be available. It ships with normal OBS
  Studio builds.
- Presets are stored in OBS's module config as `presets.ini`.
- The plugin writes diagnostics to `propresenter-lyrics.log` in OBS's module
  config folder. The file is capped at about 256 KB and trimmed to the newest
  roughly 192 KB when it grows past that limit.
- The overlay embeds `textFit` from `strml/textFit` for the `Shrink to fit`
  mode. The OBS Font Size value is used as textFit's maximum font size.
- `icon/plugin-icon.svg` is packaged with release builds. OBS 32 exposes only
  built-in source icon categories to plugins, so the source uses OBS's built-in
  text icon in the Sources list. A true custom SVG source icon would require
  OBS itself to add custom icon loading for `OBS_ICON_TYPE_CUSTOM`.
- On Apple Silicon, macOS requires plugins to be signed. Release builds should
  be signed and notarized with an Apple Developer ID certificate.

## Testing

### Discovery checks

On macOS, run the standalone discovery checks (no OBS process required):

```sh
clang++ -std=c++17 -Wall -Wextra -Werror tests/stage-discovery.cpp -o /tmp/stage-discovery-test
/tmp/stage-discovery-test
# With a running ProPresenter 7 Stage Display on the local network:
/tmp/stage-discovery-test --live
```

The live check discovers services, reconstructs each selection from its saved
identity, resolves it again, and verifies that an absent instance does not reuse
a stale endpoint. It does not authenticate or alter ProPresenter content.

### Connection responsiveness checks

To run the connection regression tests on macOS:

```sh
cmake -S . -B build/macos-arm64 -DPROPRESENTER_BUILD_TESTS=ON
cmake --build build/macos-arm64
DYLD_LIBRARY_PATH=/Applications/OBS.app/Contents/Frameworks ctest --test-dir build/macos-arm64 --output-on-failure
```

These tests exercise stalled handshakes, idle WebSockets, stalled HTTP responses,
unavailable hosts, cancelled DNS/service resolution, rapid settings updates, and
recovery to a working server.

### Validation status and implementation limits

The Apple Silicon build has passed the automated discovery and connection
responsiveness checks. Local ProPresenter discovery and WebSocket access were
verified, and the user confirmed that the reconnect fix works in OBS. The tests
check that settings updates return promptly, cancellation finishes within
300 ms, and the worker can recover from a missing service to a working server.
This is a regression-test threshold, not a guaranteed frame-time bound.
Windows discovery and the reconnect changes have not yet been tested on Windows.

On systems without Bonjour, manual hostname lookup uses the system resolver on
the connection worker. Source destruction may still wait for that system lookup;
settings updates do not join it. Numeric IP addresses avoid hostname lookup.

See [CHANGELOG.md](CHANGELOG.md) for release notes.
