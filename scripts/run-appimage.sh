#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) Opinsys Oy 2026

set -euo pipefail

image="${1:-}"
[[ -n "$image" ]] || {
  printf 'usage: run-appimage.sh <AppImage> [arguments...]\n' >&2
  exit 1
}
shift
[[ -f "$image" ]] || {
  printf 'AppImage not found: %s\n' "$image" >&2
  exit 1
}

work="$(mktemp -d "${TMPDIR:-/tmp}/puu-install-appimage.XXXXXX")"
runnable="$work/AppImage"
trap 'rm -rf -- "$work"' EXIT
cp -- "$image" "$runnable"
printf '\0\0\0' | dd of="$runnable" bs=1 seek=8 count=3 conv=notrunc status=none
chmod 700 "$runnable"
TMPDIR="$work" APPIMAGE_EXTRACT_AND_RUN=1 "$runnable" "$@"
