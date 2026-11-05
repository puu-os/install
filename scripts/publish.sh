#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) Opinsys Oy 2026
#
# Stage locally, wait for trusted npm publishing, and finalize the release.
# Run `make package` first, then push the signed tag yourself.

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=project-version.sh
source "$script_dir/project-version.sh"
# shellcheck source=sha256.sh
source "$script_dir/sha256.sh"

FILE="${FILE:-file}"
GH="${GH:-gh}"
NODE="${NODE:-node}"
NPM="${NPM:-npm}"

if command -v sha256sum >/dev/null 2>&1; then
  SHA256=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then
  SHA256=(shasum -a 256)
else
  die "neither sha256sum nor shasum is available"
fi

platforms=(
  linux-x64
  linux-arm64
)

cleanup() {
  local status=$?

  if [[ -n "${workdir:-}" ]]; then
    rm -rf "$workdir"
  fi
  return "$status"
}

trap cleanup EXIT

die() {
  printf '%s\n' "$1" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

latest_semver_tag() {
  local tag

  while IFS= read -r tag; do
    if [[ "$tag" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
      printf '%s\n' "$tag"
      return
    fi
  done < <(git tag --merged HEAD --sort=-v:refname)
  return 1
}

pack_npm_package() {
  local package_dir="$1"
  local package_name package_version archive

  package_name="$($NODE -p "require('$package_dir/package.json').name")"
  package_version="$($NODE -p "require('$package_dir/package.json').version")"
  [[ "$($NODE -p "require('$package_dir/package.json').private === true")" != true ]] \
    || die "$package_name is marked private"
  [[ "$package_version" == "$version" ]] \
    || die "$package_name version $package_version does not match $version"
  archive="$(cd "$package_dir" && $NPM pack --pack-destination "$workdir/release" --silent)"
  archive="${archive##*$'\n'}"
  [[ "$archive" == *.tgz && "$archive" != */* && -f "$workdir/release/$archive" ]] \
    || die "npm pack did not create one tarball for $package_name"
  write_sha256_sidecar "$workdir/release/$archive"
}

github_retry() {
  local attempt
  local error_file="$workdir/gh-retry-error"

  for attempt in {1..6}; do
    if "$@" 2>"$error_file"; then
      cat "$error_file" >&2
      return 0
    fi
    cat "$error_file" >&2
    if ((attempt == 6)) || ! grep -Eq 'HTTP 502|HTTP 503|HTTP 429' "$error_file"; then
      return 1
    fi
    printf 'transient GitHub error (attempt %s/6), retrying...\n' "$attempt" >&2
    sleep $((attempt * 2))
  done
}

github_release_exists() {
  local attempt
  local error_file="$workdir/gh-release-error"

  for attempt in {1..6}; do
    if $GH release view "$version" --json isDraft,tagName,assets \
      >"$workdir/gh-release.json" 2>"$error_file"; then
      return 0
    fi
    if grep -Eq '^release not found$|HTTP 404' "$error_file"; then
      return 1
    fi
    if ((attempt == 6)) || ! grep -Eq 'HTTP 502|HTTP 503|HTTP 429' "$error_file"; then
      cat "$error_file" >&2
      die "cannot query GitHub release $version"
    fi
    printf 'transient GitHub error querying release %s (attempt %s/6), retrying...\n' \
      "$version" "$attempt" >&2
    sleep $((attempt * 2))
  done
}

stage_github_release() {
  local asset name downloaded
  local missing=()
  local assets=()
  for pattern in "$workdir"/release/*.AppImage "$workdir"/release/*.sha256 \
    "$workdir"/release/*.commit "$workdir"/release/*.tgz "$workdir"/release/SHA256SUMS; do
    [[ -f "$pattern" ]] && assets+=("$pattern")
  done

  if github_release_exists; then
    $NODE -e '
      const release = require(process.argv[1]);
      if (!release.isDraft || release.tagName !== process.argv[2]) process.exit(1);
    ' "$workdir/gh-release.json" "$version" \
      || die "release $version exists but is not a draft for this tag"
    github_retry "$GH" release edit "$version" --notes-file "$notes"
    for asset in "${assets[@]}"; do
      name="${asset##*/}"
      if $NODE -e '
        const release = require(process.argv[1]);
        process.exit(release.assets.some(asset => asset.name === process.argv[2]) ? 0 : 1);
      ' "$workdir/gh-release.json" "$name"; then
        mkdir -p "$workdir/existing"
        github_retry "$GH" release download "$version" --pattern "$name" --dir "$workdir/existing"
        downloaded="$workdir/existing/$name"
        if [[ ! -f "$downloaded" ]] || ! cmp -s "$asset" "$downloaded"; then
          die "existing release asset differs: $name"
        fi
      else
        missing+=("$asset")
      fi
    done
    if ((${#missing[@]} > 0)); then
      github_retry "$GH" release upload "$version" "${missing[@]}"
    fi
    return
  fi

  github_retry "$GH" release create "$version" "${assets[@]}" \
    --draft --notes-file "$notes" --title "puu-install $version" --verify-tag
}

wait_for_npm_workflow() {
  local run_info run_id run_status previous_run attempt pipeline_url conclusion
  local run_query="[.[] | select(.displayTitle == \"Publish npm $version\")][0] | if . == null then \"\" else \"\(.databaseId) \(.status)\" end"

  run_info="$(github_retry "$GH" run list --workflow publish-npm.yml --branch main \
    --event workflow_dispatch --limit 100 --json databaseId,displayTitle,status --jq "$run_query")"
  read -r run_id run_status <<<"$run_info"
  if [[ -z "$run_id" || "$run_status" == completed ]]; then
    previous_run="${run_id:-0}"
    "$GH" workflow run publish-npm.yml --ref main -f "version=$version" \
      || die "cannot dispatch npm publishing; retry: make publish VERSION=$version"
    run_id=
    for attempt in {1..60}; do
      run_id="$(github_retry "$GH" run list --workflow publish-npm.yml --branch main \
        --event workflow_dispatch --limit 100 --json databaseId,displayTitle \
        --jq "[.[] | select(.displayTitle == \"Publish npm $version\" and .databaseId > $previous_run)][0].databaseId // empty")"
      [[ -z "$run_id" ]] || break
      ((attempt < 60)) || die "cannot find dispatched npm publish pipeline; retry: make publish VERSION=$version"
      sleep 2
    done
  fi
  pipeline_url="$(github_retry "$GH" run view "$run_id" --json url --jq .url)"
  [[ "$pipeline_url" == https://* ]] \
    || die "cannot determine npm publish pipeline URL; retry: make publish VERSION=$version"
  printf 'waiting for npm publish pipeline: %s\n' "$pipeline_url"
  while :; do
    run_info="$(github_retry "$GH" run view "$run_id" --json status,conclusion \
      --jq '"\(.status) \(.conclusion // "")"')"
    read -r run_status conclusion <<<"$run_info"
    case "$run_status" in
      completed)
        [[ "$conclusion" == success ]] && break
        die "npm publish pipeline $run_id ended with status ${conclusion:-unknown}; retry: make publish VERSION=$version"
        ;;
      queued|waiting|requested|pending|in_progress) sleep 10 ;;
      *) die "npm publish pipeline $run_id ended with status $run_status; retry: make publish VERSION=$version" ;;
    esac
  done
}

complete_release() {
  local status=0 is_draft

  is_draft="$($NODE -p 'require(process.argv[1]).isDraft' "$workdir/gh-release.json")"
  NPM="$NPM" NODE="$NODE" scripts/publish-npm-provenance.sh verify "$version" "$workdir/release"
  NPM="$NPM" NODE="$NODE" scripts/publish-npm-provenance.sh check "$version" "$workdir/release" || status=$?
  if ((status != 0)); then
    ((status == 1)) || exit "$status"
    [[ "$is_draft" == true ]] || die "public release $version has unpublished npm packages"
    wait_for_npm_workflow
    NPM="$NPM" NODE="$NODE" scripts/publish-npm-provenance.sh check "$version" "$workdir/release"
  fi

  if [[ "$is_draft" == true ]]; then
    github_retry "$GH" release edit "$version" --draft=false
  fi
  printf 'published puu-install %s\n' "$version"
}

for command in "$GH" "$NODE" "$NPM" git tar "$FILE"; do
  require_command "$command"
done

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" \
  || die "not inside a Git repository"
cd "$repo_root"

"$GH" repo view >/dev/null 2>&1 || die "cannot access the GitHub repository"

git symbolic-ref --quiet --short HEAD >/dev/null \
  || die "HEAD is detached; check out the release branch before publishing"
[[ -z "$(git status --porcelain)" ]] || die "working directory is not clean"

version="${1:-}"
if [[ -z "$version" ]]; then
  version="$(latest_semver_tag)" || die "no semver tag reachable from HEAD"
fi
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "invalid version: $version"

project_version="$(read_project_version CMakeLists.txt)"
[[ "$project_version" == "$version" ]] \
  || die "CMakeLists.txt version $project_version does not match $version"

package_version="$($NODE -p "require('./package.json').version")"
[[ "$version" == "$package_version" ]] \
  || die "tag $version package.json version is $package_version"

tag_commit="$(git rev-parse "$version^{commit}" 2>/dev/null)" \
  || die "tag $version does not exist"
git verify-tag "$version" >/dev/null \
  || die "tag $version does not have a valid signature"
git merge-base --is-ancestor "$tag_commit" HEAD \
  || die "tag $version is not an ancestor of HEAD"

if [[ "$tag_commit" != "$(git rev-parse HEAD)" ]]; then
  if ! git diff --quiet "$tag_commit" HEAD -- npm package.json; then
    die "packaged sources changed since tag $version"
  fi
fi

workdir="$(mktemp -d)"
mkdir -p "$workdir/packages" "$workdir/release"
notes="$workdir/release-notes.md"
git for-each-ref --format='%(contents:subject)%0a%0a%(contents:body)' \
  "refs/tags/$version" >"$notes"

if github_release_exists; then
  printf 'verifying staged puu-install %s\n' "$version"
  github_retry "$GH" release download "$version" --pattern '*.tgz' \
    --pattern '*.tgz.sha256' --dir "$workdir/release"
  complete_release
  exit 0
fi

local_tag="$(git rev-parse "refs/tags/$version")"
remote_output="$(git ls-remote --refs origin "refs/tags/$version")" \
  || die "cannot query tag $version from origin"
remote_tag="${remote_output%%$'\t'*}"
[[ -n "$remote_tag" && "$remote_tag" == "$local_tag" ]] \
  || die "push tag $version to origin before publishing"

remote_main="$(git ls-remote origin refs/heads/main | awk '{print $1}')"
if ! git cat-file -e "$remote_main^{commit}" >/dev/null 2>&1; then
  git fetch --quiet origin main \
    || die "cannot fetch origin main to verify the release commit"
  git cat-file -e "$remote_main^{commit}" >/dev/null 2>&1 \
    || die "cannot verify origin/main contains the release commit"
fi
git merge-base --is-ancestor "$tag_commit" "$remote_main" \
  || die "push the release commit to origin/main before staging the release"

printf 'validating packaged artifacts from tag %s\n' "$version"
missing=()
npm_package_dirs=()
for platform in "${platforms[@]}"; do
  binary="puu-install.AppImage"
  case "$platform" in
    linux-arm64) expected_arch=aarch64 ;;
    linux-x64)   expected_arch=x86-64 ;;
  esac

  source_bin="npm/$platform/bin/$binary"
  asset="puu-install-$version-$platform.AppImage"
  asset_path="artifacts/$asset"
  checksum_path="$asset_path.sha256"
  commit_path="$asset_path.commit"
  if [[ ! -f "$source_bin" || ! -f "$asset_path" || \
    ! -f "$checksum_path" || ! -f "$commit_path" ]]; then
    missing+=("$platform binary, artifact, checksum, or commit provenance")
    continue
  fi

  read -r expected_digest expected_asset extra <"$checksum_path"
  [[ -n "$expected_digest" && "$expected_asset" == "$asset" && -z "${extra:-}" ]] \
    || die "invalid checksum file: $checksum_path"
  actual_digest="$("${SHA256[@]}" "$asset_path" | awk '{print $1}')"
  [[ "$actual_digest" == "$expected_digest" ]] \
    || die "checksum mismatch: $asset_path"
  [[ "$(<"$commit_path")" == "$tag_commit" ]] \
    || die "$asset was not built from tag $version"

  description="$("$FILE" -b "$source_bin")"
  [[ "${description,,}" == *"$expected_arch"* ]] \
    || die "$source_bin has the wrong architecture: $description"

  magic="$(od -An -tx1 -j8 -N3 "$source_bin" | tr -d ' \n')"
  [[ "$magic" == 414902 || "$magic" == 000000 ]] \
    || die "$source_bin is not a valid AppImage (magic: $magic)"
  cmp "$source_bin" "$asset_path" >/dev/null \
    || die "$source_bin differs from $asset_path"

  package_dir="$workdir/packages/$platform"
  cp -a "npm/$platform" "$package_dir"
  mkdir -p "$package_dir/bin"
  cp "$source_bin" "$package_dir/bin/$binary"
  chmod 755 "$package_dir/bin/$binary"
  cp "$asset_path" "$checksum_path" "$commit_path" "$workdir/release/"
  npm_package_dirs+=("$package_dir")
done

if ((${#missing[@]} > 0)); then
  printf 'missing platform release inputs:\n' >&2
  printf '  %s\n' "${missing[@]}" >&2
  die "run 'make package' before publish"
fi

npm_package_dirs+=("$repo_root")

printf 'packing npm packages locally\n'
for package_dir in "${npm_package_dirs[@]}"; do
  pack_npm_package "$package_dir"
done
NPM="$NPM" NODE="$NODE" scripts/publish-npm-provenance.sh preflight "$version" "$workdir/release"

(
  cd "$workdir/release"
  rm -f SHA256SUMS
  for f in *.AppImage *.tgz; do
    [[ -f "$f" ]] || continue
    "${SHA256[@]}" "$f"
  done | sort -k2 > SHA256SUMS
)

{
  printf '\n### SHA-256 Checksums\n\n```\n'
  cat "$workdir/release/SHA256SUMS"
  printf '```\n'
} >>"$notes"

stage_github_release
github_release_exists || die "cannot find newly staged GitHub release $version"
complete_release
