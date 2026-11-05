#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) Opinsys Oy 2026

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=project-version.sh
source "$script_dir/project-version.sh"

if [[ "$(uname -s)" == Darwin ]]; then
  CONTAINER="${CONTAINER:-container}"
else
  CONTAINER="${CONTAINER:-docker}"
fi

publish_file() {
  local source="$1" destination="$2"
  local temporary

  mkdir -p "$(dirname "$destination")"
  temporary="$(mktemp "$destination.XXXXXX")"
  if ! cp -p "$source" "$temporary"; then
    rm -f "$temporary"
    return 1
  fi
  if ! mv -f "$temporary" "$destination"; then
    rm -f "$temporary"
    return 1
  fi
}

for command in "$CONTAINER" cp git id mkdir mktemp mv sed tar; do
  command -v "$command" >/dev/null 2>&1 || {
    printf 'required command not found: %s\n' "$command" >&2
    exit 1
  }
done

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  printf 'not inside a Git repository\n' >&2
  exit 1
}
cd "$repo_root"
container_kind="$(basename -- "$CONTAINER")"
if [ "$container_kind" = container ]; then
  "$CONTAINER" system status >/dev/null 2>&1 || "$CONTAINER" system start
else
  "$CONTAINER" info >/dev/null
fi
[[ -z "$(git status --porcelain)" ]] || {
  printf 'working tree must be clean for release packaging\n' >&2
  exit 1
}
head_commit="$(git rev-parse HEAD)"
version="$(git show "$head_commit:CMakeLists.txt" | sed -n 's/^[[:space:]]*VERSION[[:space:]]*\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -1)"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  printf 'invalid CMakeLists.txt version in HEAD: %s\n' "$version" >&2
  exit 1
}

release_tree="$(mktemp -d)"
stage="$(mktemp -d)"
cleanup() {
  local status=$?
  rm -rf -- "$release_tree" "$stage"
  return "$status"
}
trap cleanup EXIT
git archive "$head_commit" | tar -x -C "$release_tree"

uid="$(id -u)"
gid="$(id -g)"

targets=('linux/amd64 linux-x64 amd64' 'linux/arm64 linux-arm64 arm64')
for target in "${targets[@]}"; do
  read -r container_platform package_platform image_arch <<<"$target"
  image="puu-install-appimage-build:$image_arch"
  asset_name="puu-install-$version-$package_platform.AppImage"
  asset="$stage/artifacts/$asset_name"

  printf 'building container for %s\n' "$package_platform"
  "$CONTAINER" build \
    --platform "$container_platform" \
    --tag "$image" \
    --file "$release_tree/appimage/Dockerfile" \
    "$release_tree/appimage"

  printf 'packaging %s\n' "$package_platform"
  "$CONTAINER" run --rm \
    --platform "$container_platform" \
    --user "$uid:$gid" \
    --env HOME=/tmp/home \
    --env PACKAGE_OUTPUT_ROOT=/out \
    --tmpfs /tmp/home:rw,exec,mode=1777 \
    --mount "type=bind,source=$repo_root,target=/src,readonly" \
    --mount "type=bind,source=$release_tree/scripts,target=/src/scripts,readonly" \
    --mount "type=bind,source=$stage,target=/out" \
    --workdir /src \
    "$image" \
    scripts/package.sh "$package_platform"

  [[ "$(git rev-parse HEAD)" == "$head_commit" ]] || {
    printf 'HEAD changed during release packaging\n' >&2
    exit 1
  }
  [[ -f "$asset" && -f "$asset.sha256" && -f "$asset.commit" ]] || {
    printf 'packaging did not produce all files for %s\n' "$asset_name" >&2
    exit 1
  }
  [[ "$(<"$asset.commit")" == "$head_commit" ]] || {
    printf '%s was not built from %s\n' "$asset_name" "$head_commit" >&2
    exit 1
  }
  "$CONTAINER" run --rm \
    --platform "$container_platform" \
    --user "$uid:$gid" \
    --mount "type=bind,source=$stage,target=/out,readonly" \
    --workdir /out/artifacts \
    "$image" \
    sh -c "sha256sum --check \"\$1.sha256\" && cmp --silent \"/out/npm/\$2/bin/puu-install.AppImage\" \"\$1\" && file -b \"\$1\"" \
    sh "$asset_name" "$package_platform"
done

[[ "$(git rev-parse HEAD)" == "$head_commit" ]] || {
  printf 'HEAD changed during release packaging\n' >&2
  exit 1
}
for target in "${targets[@]}"; do
  read -r _ package_platform _ <<<"$target"
  asset_name="puu-install-$version-$package_platform.AppImage"
  publish_file "$stage/npm/$package_platform/bin/puu-install.AppImage" \
    "$repo_root/npm/$package_platform/bin/puu-install.AppImage"
  for suffix in '' .sha256 .commit; do
    publish_file "$stage/artifacts/$asset_name$suffix" \
      "$repo_root/artifacts/$asset_name$suffix"
  done
done
(
  cd "$stage/artifacts"
  sort -k2 puu-install-"$version"-*.AppImage.sha256 > SHA256SUMS
)
publish_file "$stage/artifacts/SHA256SUMS" "$repo_root/artifacts/SHA256SUMS"

printf 'packaged linux-x64 and linux-arm64 from %s\n' "$head_commit"
