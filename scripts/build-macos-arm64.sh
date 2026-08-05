#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

find_libobs_config() {
  local candidates=(
    "$HOME/Developer/obs-sdk-32.1.2"
    "$HOME/Developer/obs-sdk"
    "$HOME/Developer/obs-studio/build_macos/libobs"
    "$HOME/Developer/obs-studio/build/libobs"
    "$ROOT_DIR/../obs-sdk"
    "$ROOT_DIR/../obs-studio/build_macos/libobs"
    "$ROOT_DIR/../obs-studio/build/libobs"
  )

  local sdk
  for sdk in "$HOME"/Developer/obs-sdk-*; do
    if [[ -d "$sdk" ]]; then
      candidates+=("$sdk")
    fi
  done

  for candidate in "${candidates[@]}"; do
    if [[ -d "$candidate" ]]; then
      find "$candidate" \( -name libobsConfig.cmake -o -name libobs-config.cmake \) -print -quit
    fi
  done
}

find_package_config() {
  local package_name="$1"
  local lower_name="$2"
  local candidates=(
    "$HOME/Developer/obs-sdk"
    "$HOME/Developer/obs-studio"
    "/opt/homebrew"
    "$ROOT_DIR/../obs-sdk"
    "$ROOT_DIR/../obs-studio"
  )

  for candidate in "${candidates[@]}"; do
    if [[ -d "$candidate" ]]; then
      find "$candidate" \( -name "${package_name}Config.cmake" -o -name "${lower_name}-config.cmake" \) -print -quit
    fi
  done
}

LIBOBS_CONFIG="${LIBOBS_CONFIG:-$(find_libobs_config)}"
SIMDE_CONFIG="${SIMDE_CONFIG:-$(find_package_config SIMDe simde)}"

if [[ -z "$LIBOBS_CONFIG" ]]; then
  cat >&2 <<'EOF'
Could not find libobsConfig.cmake.

Build and install OBS Studio development files first, then rerun this script.
Expected locations include:

  $HOME/Developer/obs-sdk
  $HOME/Developer/obs-studio

You can also pass it explicitly:

  LIBOBS_CONFIG=/path/to/libobsConfig.cmake bash scripts/build-macos-arm64.sh
EOF
  exit 1
fi

LIBOBS_DIR="$(dirname "$LIBOBS_CONFIG")"
EXTRA_CMAKE_ARGS=()

if [[ -n "$SIMDE_CONFIG" ]]; then
  SIMDE_DIR="$(dirname "$SIMDE_CONFIG")"
  EXTRA_CMAKE_ARGS+=("-DSIMDe_DIR=$SIMDE_DIR")
  echo "Using SIMDe_DIR=$SIMDE_DIR"
else
  if [[ -d /opt/homebrew/include/simde || -d /usr/local/include/simde ]]; then
    SIMDE_DIR="$ROOT_DIR/cmake/shims/SIMDe"
    EXTRA_CMAKE_ARGS+=("-DSIMDe_DIR=$SIMDE_DIR")
    echo "Using local SIMDe CMake shim at $SIMDE_DIR"
  else
    echo "SIMDeConfig.cmake and SIMDe headers were not found automatically."
    echo "Install them with: brew install simde"
  fi
fi

echo "Using libobs_DIR=$LIBOBS_DIR"

cmake --preset macos-arm64 --fresh \
  -Dlibobs_DIR="$LIBOBS_DIR" \
  "${EXTRA_CMAKE_ARGS[@]}"

cmake --build --preset macos-arm64

rm -rf "$ROOT_DIR/dist/macos-arm64/propresenter-lyrics.plugin"

cmake --install "$ROOT_DIR/build/macos-arm64" \
  --prefix "$ROOT_DIR/dist/macos-arm64"
