#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) Opinsys Oy 2026

set -euo pipefail

LINUXDEPLOY_VERSION=1-alpha-20251107-1
RUNTIME_VERSION=20251108

require_command() {
  command -v "$1" >/dev/null 2>&1 || {
    printf 'required command not found: %s\n' "$1" >&2
    exit 1
  }
}

sha256_digest() {
  local output

  if command -v sha256sum >/dev/null 2>&1; then
    output="$(sha256sum "$1")"
  else
    output="$(shasum -a 256 "$1")"
  fi
  printf '%s\n' "${output%% *}"
}

download_verified() {
  local url="$1" output="$2" checksum="$3" label="$4"
  local status temporary

  require_command curl
  mkdir -p "$(dirname "$output")"
  temporary="$(mktemp "$output.XXXXXX")"
  if curl --fail --location --show-error --silent \
    --header 'Accept: application/octet-stream' \
    --output "$temporary" "$url"; then
    :
  else
    status=$?
    rm -f "$temporary"
    return "$status"
  fi
  [[ "$(sha256_digest "$temporary")" == "$checksum" ]] || {
    rm -f "$temporary"
    printf '%s checksum failed\n' "$label" >&2
    return 1
  }
  mv "$temporary" "$output"
}

for command in cmake pkg-config; do
  require_command "$command"
done
if ! command -v sha256sum >/dev/null 2>&1 && \
  ! command -v shasum >/dev/null 2>&1; then
  printf 'required command not found: sha256sum or shasum\n' >&2
  exit 1
fi

[[ "$(uname -s)" == Linux ]] || {
  printf 'AppImages must be built on Linux\n' >&2
  exit 1
}

case "$(uname -m)" in
x86_64)
  platform=linux-x64
  appimage_arch=x86_64
  linuxdeploy_checksum=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
  runtime_checksum=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
  ;;
aarch64|arm64)
  platform=linux-arm64
  appimage_arch=aarch64
  linuxdeploy_checksum=620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff
  runtime_checksum=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444
  ;;
*)
  printf 'unsupported architecture: %s\n' "$(uname -m)" >&2
  exit 1
  ;;
esac

requested_platform="${1:-$platform}"
[[ "$requested_platform" == "$platform" ]] || {
  printf '%s AppImages must be built on %s (this host is %s)\n' \
    "$requested_platform" "$requested_platform" "$platform" >&2
  exit 1
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$repo_root/build-appimage"
appdir="$build/AppDir"
output="${2:-$build/puu-install.AppImage}"
[[ "$output" == /* ]] || output="$PWD/$output"
# shellcheck source=project-version.sh
source "$repo_root/scripts/project-version.sh"
version="$(read_project_version "$repo_root/CMakeLists.txt")"

if [[ -n "${LINUXDEPLOY:-}" ]]; then
  linuxdeploy="$LINUXDEPLOY"
  require_command "$linuxdeploy"
  linuxdeploy="$(command -v "$linuxdeploy")"
else
  cache="${XDG_CACHE_HOME:-$HOME/.cache}/puu-install/linuxdeploy/$LINUXDEPLOY_VERSION"
  linuxdeploy="$cache/linuxdeploy-$appimage_arch.AppImage"
  if [[ ! -f "$linuxdeploy" ]] || \
    [[ "$(sha256_digest "$linuxdeploy")" != "$linuxdeploy_checksum" ]]; then
    url="https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_VERSION/linuxdeploy-$appimage_arch.AppImage"
    printf 'downloading %s\n' "$url"
    download_verified "$url" "$linuxdeploy" "$linuxdeploy_checksum" linuxdeploy
  fi
  chmod 755 "$linuxdeploy"
fi

runtime_cache="${XDG_CACHE_HOME:-$HOME/.cache}/puu-install/appimage-runtime/$RUNTIME_VERSION"
runtime="$runtime_cache/runtime-$appimage_arch"
if [[ ! -f "$runtime" ]] || \
  [[ "$(sha256_digest "$runtime")" != "$runtime_checksum" ]]; then
  url="https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-$appimage_arch"
  printf 'downloading %s\n' "$url"
  download_verified "$url" "$runtime" "$runtime_checksum" 'AppImage runtime'
fi

rm -rf "$build"
mkdir -p "$(dirname "$output")"
cmake -S "$repo_root" -B "$build/cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build/cmake" --parallel

argtable3_lib="$(find "$build/cmake/_deps/argtable3-build" -name "libargtable3*.so*" -type f | head -1)"
[[ -n "$argtable3_lib" && -f "$argtable3_lib" ]] || {
  printf 'libargtable3 shared library not found in %s\n' "$build/cmake/_deps/argtable3-build" >&2
  exit 1
}

ARCH="$appimage_arch" \
LDAI_NO_APPSTREAM=1 \
LDAI_RUNTIME_FILE="$runtime" \
LDAI_OUTPUT="$output" \
LINUXDEPLOY_OUTPUT_VERSION="$version" \
"$repo_root/scripts/run-appimage.sh" "$linuxdeploy" \
  --appdir "$appdir" \
  --executable "$build/cmake/src/puu-install" \
  --library "$argtable3_lib" \
  --custom-apprun "$repo_root/appimage/AppRun" \
  --desktop-file "$repo_root/appimage/puu-install.desktop" \
  --icon-file "$repo_root/appimage/puu-install.svg" \
  --output appimage

"$repo_root/scripts/run-appimage.sh" "$output" --version >/dev/null
printf 'built %s\n' "$output"
