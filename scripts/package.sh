#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) Opinsys Oy 2026

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=project-version.sh
source "$script_dir/project-version.sh"

FILE="${FILE:-file}"
NODE="${NODE:-node}"

require_command() {
  command -v "$1" >/dev/null 2>&1 || {
    printf 'required command not found: %s\n' "$1" >&2
    exit 1
  }
}

for command in "$FILE" "$NODE" git tar; do
  require_command "$command"
done
if ! command -v sha256sum >/dev/null 2>&1 && \
  ! command -v shasum >/dev/null 2>&1; then
  printf 'required command not found: sha256sum or shasum\n' >&2
  exit 1
fi

[[ "$(uname -s)" == Linux ]] || {
  printf 'AppImages must be packaged on Linux\n' >&2
  exit 1
}

case "$(uname -m)" in
x86_64)
  host_platform=linux-x64
  file_arch=x86-64
  ;;
aarch64|arm64)
  host_platform=linux-arm64
  file_arch=aarch64
  ;;
*)
  printf 'unsupported architecture: %s\n' "$(uname -m)" >&2
  exit 1
  ;;
esac
platform="${1:-$host_platform}"
[[ "$platform" == "$host_platform" ]] || {
  printf '%s must be packaged on %s (this host is %s)\n' \
    "$platform" "$platform" "$host_platform" >&2
  exit 1
}

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  printf 'not inside a Git repository\n' >&2
  exit 1
}
cd "$repo_root"
output_root="${PACKAGE_OUTPUT_ROOT:-$repo_root}"
[[ "$output_root" == /* ]] || output_root="$repo_root/$output_root"

work="$(mktemp -d)"
cleanup() {
  local status=$?
  rm -rf "$work"
  return "$status"
}
trap cleanup EXIT

head_commit="$(git rev-parse HEAD)"
git archive "$head_commit" | tar -x -C "$work"
version="$(read_project_version "$work/CMakeLists.txt")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  printf 'invalid CMakeLists.txt version in HEAD: %s\n' "$version" >&2
  exit 1
}
package_version="$("$NODE" -e \
  'process.stdout.write(require(process.argv[1]).version)' \
  "$work/package.json")"
[[ "$package_version" == "$version" ]] || {
  printf 'package.json version %s does not match CMakeLists.txt %s in HEAD\n' \
    "$package_version" "$version" >&2
  exit 1
}

package_path="npm/$platform"
archived_package="$work/$package_path/package.json"
[[ -f "$archived_package" ]] || {
  printf 'HEAD is missing %s/package.json\n' "$package_path" >&2
  exit 1
}
platform_version="$("$NODE" -e \
  'process.stdout.write(require(process.argv[1]).version)' \
  "$archived_package")"
[[ "$platform_version" == "$version" ]] || {
  printf '%s version %s does not match HEAD version %s\n' \
    "$package_path" "$platform_version" "$version" >&2
  exit 1
}
printf 'packaging HEAD (%s) for %s\n' "$version" "$platform"

asset="puu-install-$version-$platform.AppImage"
source_image="$work/$asset"
(
  cd "$work"
  scripts/build-appimage.sh "$platform" "$source_image"
)

description="$($FILE -b "$source_image")"
printf '%s\n' "$description"
[[ "${description,,}" == *"$file_arch"* ]] || {
  printf 'AppImage has the wrong architecture: %s\n' "$description" >&2
  exit 1
}

package_output="$output_root/npm/$platform"
artifact_output="$output_root/artifacts"
rm -rf "${package_output:?}/bin"
mkdir -p "$package_output/bin" "$artifact_output"
cp "$source_image" "$package_output/bin/puu-install.AppImage"
chmod 755 "$package_output/bin/puu-install.AppImage"
scripts/run-appimage.sh "$package_output/bin/puu-install.AppImage" --version >/dev/null
cp "$source_image" "$artifact_output/$asset"

if command -v sha256sum >/dev/null 2>&1; then
  digest="$(sha256sum "$artifact_output/$asset")"
else
  digest="$(shasum -a 256 "$artifact_output/$asset")"
fi
printf '%s  %s\n' "${digest%% *}" "$asset" >"$artifact_output/$asset.sha256"
printf '%s\n' "$head_commit" >"$artifact_output/$asset.commit"
printf 'packaged %s -> %s/bin/puu-install.AppImage and %s/%s\n' \
  "$platform" "$package_output" "$artifact_output" "$asset"
