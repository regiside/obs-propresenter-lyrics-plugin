#!/usr/bin/env bash
set -euo pipefail

OBS_STUDIO_DIR="${OBS_STUDIO_DIR:-$HOME/Developer/obs-studio}"
OBS_VERSION="${OBS_VERSION:-}"
SDK_PREFIX="${SDK_PREFIX:-$HOME/Developer/obs-sdk}"

if [[ -z "$OBS_VERSION" ]]; then
  OBS_VERSION="$(awk '/OBS [0-9]+\.[0-9]+\.[0-9]+ \(mac\)/ { print $3; exit }' "$HOME/Library/Application Support/obs-studio/logs/"*.txt 2>/dev/null || true)"
fi

if [[ -z "$OBS_VERSION" ]]; then
  echo "Could not detect installed OBS version. Pass OBS_VERSION=32.1.2 explicitly." >&2
  exit 1
fi

if [[ ! -d "$OBS_STUDIO_DIR/.git" ]]; then
  echo "OBS source checkout not found at: $OBS_STUDIO_DIR" >&2
  exit 1
fi

cd "$OBS_STUDIO_DIR"

echo "Building OBS SDK for OBS $OBS_VERSION"
git fetch --tags
git checkout "$OBS_VERSION"
git submodule update --init --recursive

cmake --preset macos --fresh \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_INSTALL_PREFIX="$SDK_PREFIX"

cmake --build build_macos --config RelWithDebInfo

cmake --install build_macos \
  --prefix "$SDK_PREFIX" \
  --config RelWithDebInfo

find "$SDK_PREFIX" \( -name libobsConfig.cmake -o -name libobs-config.cmake \) -print

