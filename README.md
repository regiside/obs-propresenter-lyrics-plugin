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
- Connects to ProPresenter at `ws://<ip>:<port>/stagedisplay`.
- Authenticates with the configured ProPresenter password.
- Requests Stage Display frame values for current slide text, next slide text,
  notes, or stage message.
- Uses `/v1/status/slide` as an optional HTTP fallback.
- Renders the lyric overlay internally through OBS's built-in Browser Source.
- Supports presets and controls for color, opacity, font family, font size,
  font weight, italic, letter spacing, alignment, line height, max lines,
  shrink-to-fit text sizing, text shadow, line background, and crossfade
  duration.

## ProPresenter Setup

In ProPresenter, enable network control and Stage Display API access. Use the
same IP address, port, and password in the OBS source properties.

The default port is `50001`. If your ProPresenter network settings use a
different port, enter that value in the OBS source.

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
