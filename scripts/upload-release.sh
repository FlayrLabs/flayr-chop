#!/usr/bin/env bash
# Adds the macOS DMG and SHA256SUMS.txt to the draft release, after verify-release-assets.sh has
# vouched for every CI-built file; then checks the draft holds exactly what was checksummed.
#
#   scripts/upload-release.sh <tag> <tag commit sha> <version> <local DMG>
#
# Run by release-macos.sh --upload (from the verified tag tree). Fails closed: a published (not
# draft) release, any verification failure, an unexpected file on the draft afterwards, a digest
# that doesn't match, or an uploaded SHA256SUMS.txt that differs from the local one stops it.
set -euo pipefail

[[ $# -eq 4 ]] || { echo "usage: $0 <tag> <tag commit sha> <version> <local DMG>" >&2; exit 2; }
TAG="$1" TAG_SHA="$2" VERSION="$3" DMG="$4"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "error: $*" >&2; exit 1; }
command -v jq >/dev/null || die "jq is not installed"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

REPO=FlayrLabs/flayr-chop # pinned
# gh runs sealed too: GH_HOST/GH_REPO-style settings are refused, not inherited.
for var in GH_HOST GH_REPO GH_CONFIG_DIR GH_ENTERPRISE_TOKEN GITHUB_ENTERPRISE_TOKEN HTTPS_PROXY HTTP_PROXY ALL_PROXY; do
    [[ -z "${!var:-}" ]] || die "$var is set; unset it for a release"
done
GH_BIN="$(command -v gh)" || die "gh not found"
gh() { env -i "PATH=$PATH" "HOME=$HOME" ${GH_TOKEN:+"GH_TOKEN=$GH_TOKEN"} ${GITHUB_TOKEN:+"GITHUB_TOKEN=$GITHUB_TOKEN"} "$GH_BIN" "$@"; }

# Only ever add to a draft: replacing files on a published release would swap what
# people already downloaded and verified.
[[ "$(gh release view "$TAG" --repo "$REPO" --json isDraft --jq .isDraft)" == "true" ]] \
    || die "release $TAG is already published (or missing); refusing to change its files"

# Verify the CI assets (strict provenance, bound to the tag's own release.yml run) and
# checksum exactly the verified bytes plus our DMG. Nothing is uploaded unless all of
# it passes. The verifier comes from the verified tag tree, not the working tree.
sums_dir="$(mktemp -d "$WORK/sums.XXXXXX")"
"$SRC/scripts/verify-release-assets.sh" "$TAG" "$TAG_SHA" "$VERSION" "$DMG" "$sums_dir" \
    || die "release assets did not verify; nothing was uploaded"
cat "$sums_dir/SHA256SUMS.txt"

gh release upload "$TAG" "$DMG" "$sums_dir/SHA256SUMS.txt" --clobber --repo "$REPO" # a draft, checked above

# The draft must now hold exactly the allowed files, each matching SHA256SUMS.txt, and the
# uploaded SHA256SUMS.txt must be byte for byte the one written here.
assets="$(gh release view "$TAG" --repo "$REPO" --json assets --jq '.assets')"
while IFS= read -r name; do
    case "$name" in
        "FlayrChop-$VERSION-Windows-Setup.exe"|"FlayrChop-$VERSION-Linux-x64.tar.gz"|"$(basename "$DMG")"|SHA256SUMS.txt) ;;
        *) die "unexpected asset on the draft $TAG after upload: $name; do not publish" ;;
    esac
done < <(jq -r '.[].name' <<< "$assets")
while read -r sum name; do
    [[ "$(jq -r --arg n "$name" '.[] | select(.name == $n) | .digest // empty' <<< "$assets")" == "sha256:$sum" ]] \
        || die "$name on the draft no longer matches SHA256SUMS.txt; do not publish"
done < "$sums_dir/SHA256SUMS.txt"
check_dir="$(mktemp -d "$WORK/check.XXXXXX")"
gh release download "$TAG" --repo "$REPO" --pattern SHA256SUMS.txt --dir "$check_dir"
cmp -s "$check_dir/SHA256SUMS.txt" "$sums_dir/SHA256SUMS.txt" || die "the uploaded SHA256SUMS.txt differs from the one written here"
echo "==> Uploaded $(basename "$DMG") and SHA256SUMS.txt to the draft release $TAG"
