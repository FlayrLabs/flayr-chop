#!/usr/bin/env bash
# Builds the macOS release (universal AU + VST3), signs it with a Developer ID,
# notarizes and staples it, and wraps it in a drag-to-install DMG.
#
#   scripts/release-macos.sh            # build dist/FlayrChop-<version>-macOS.dmg
#   scripts/release-macos.sh --upload   # ...and attach it to the GitHub release v<version>
#
# Needs a "Developer ID Application" identity in the keychain and a notarytool
# keychain profile (xcrun notarytool store-credentials <profile> ...).
#
# Only signs a tagged release: HEAD must sit exactly on a v<version> tag that matches
# CMakeLists.txt and the same tag on GitHub. The source is not a checkout: it is
# `git archive` of the tag's commit, taken from a fresh bare copy of the repository,
# and the extracted files must hash to exactly the tag's tree before anything is built.
# Every git command runs sealed (env -i, no system or global config, no templates, no
# hooks, no fsmonitor, no attributes file), so hooks, templates, smudge filters,
# insteadOf rewrites or a modified working tree can't change what gets built. Nothing
# from the environment reaches the build either: configure and build run under env -i
# with a minimal PATH, and the script refuses to start if CMake/FetchContent/compiler
# overrides are set (FETCHCONTENT_SOURCE_DIR_JUCE=<unpatched JUCE>,
# for one, would otherwise reach the signed build). The result is checked for every JUCE
# patch before anything is signed, and nothing staged for signing may be a symbolic link.
set -euo pipefail

IDENTITY="${IDENTITY:-Developer ID Application: Flayr Labs LLC (NRNU83UJ68)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-flayr-notary}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(FlayrChop VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
TAG="v$VERSION"
DIST="$ROOT/dist"
DMG="$DIST/FlayrChop-$VERSION-macOS.dmg"

die() { echo "error: $*" >&2; exit 1; }

overrides="$(env | grep -E '^(FETCHCONTENT_|CMAKE_|CTEST_|CXXFLAGS|CFLAGS|CPPFLAGS|LDFLAGS|OBJCFLAGS|OBJCXXFLAGS|CC|CXX|OBJC|OBJCXX|SDKROOT|MACOSX_DEPLOYMENT_TARGET|LIBRARY_PATH|CPATH|C_INCLUDE_PATH|CPLUS_INCLUDE_PATH|DYLD_)[A-Z_]*=' | cut -d= -f1 || true)"
[[ -z "$overrides" ]] || die "build overrides are set in the environment, unset them first: $(echo $overrides)"

[[ -n "$VERSION" ]] || die "could not read the version from CMakeLists.txt"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src"
BUILD="$WORK/build"
EMPTY="$WORK/empty"
mkdir -p "$EMPTY" "$SRC"

# Sealed git: nothing from the environment, system or global config, templates or hook
# directories. Repository-local config is only ever read from $ROOT for plumbing
# (rev-parse); the archive comes from a bare copy whose config git itself just wrote.
GIT="$(command -v git)" || die "git not found"
g() {
    env -i "PATH=/usr/bin:/bin:/usr/sbin:/sbin" "HOME=$EMPTY" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
        GIT_TEMPLATE_DIR="$EMPTY" GIT_ATTR_NOSYSTEM=1 GIT_TERMINAL_PROMPT=0 \
        "$GIT" -c core.hooksPath=/dev/null -c core.fsmonitor=false -c core.attributesFile=/dev/null "$@"
}

HEAD_SHA="$(g -C "$ROOT" rev-parse --verify "HEAD^{commit}")"
TAG_SHA="$(g -C "$ROOT" rev-parse -q --verify "refs/tags/$TAG^{commit}" || true)"
[[ -n "$TAG_SHA" ]] || die "tag $TAG does not exist; tag the release commit first"
[[ "$HEAD_SHA" == "$TAG_SHA" ]] || die "HEAD is not at $TAG (HEAD $HEAD_SHA, $TAG $TAG_SHA)"
# Asked of GitHub by URL from an empty directory: no local remote or insteadOf can redirect it.
REMOTE_URL="https://github.com/FlayrLabs/flayr-chop.git"
REMOTE_SHA="$(cd "$EMPTY" && g ls-remote "$REMOTE_URL" "refs/tags/$TAG^{}" | cut -f1)"
[[ -n "$REMOTE_SHA" ]] || REMOTE_SHA="$(cd "$EMPTY" && g ls-remote "$REMOTE_URL" "refs/tags/$TAG" | cut -f1)"
[[ "$REMOTE_SHA" == "$TAG_SHA" ]] || die "$TAG on GitHub (${REMOTE_SHA:-missing}) differs from the local tag; push it first"

echo "==> Exporting $TAG ($TAG_SHA)"
g clone --quiet --bare --no-local --no-tags "file://$ROOT" "$WORK/mirror.git"
g -C "$WORK/mirror.git" fetch --quiet "file://$ROOT" "+refs/tags/$TAG:refs/tags/$TAG"
[[ "$(g -C "$WORK/mirror.git" rev-parse --verify "refs/tags/$TAG^{commit}")" == "$TAG_SHA" ]] || die "the copy's $TAG is not $TAG_SHA"
TREE="$(g -C "$WORK/mirror.git" rev-parse --verify "$TAG_SHA^{tree}")"
g -C "$WORK/mirror.git" archive --format=tar "$TAG_SHA" | tar -x -f - -C "$SRC"
# Hash what was extracted (in a throwaway index) and require exactly the tag's tree:
# export-ignore/export-subst attributes or anything else that changed a byte fails here.
g init --quiet --bare "$WORK/verify.git"
g --git-dir="$WORK/verify.git" --work-tree="$SRC" add --all --force .
[[ "$(g --git-dir="$WORK/verify.git" --work-tree="$SRC" write-tree)" == "$TREE" ]] \
    || die "the exported files don't match $TAG's tree $TREE"
[[ "$(sed -n 's/^project(FlayrChop VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")" == "$VERSION" ]] \
    || die "$TAG's CMakeLists.txt is not version $VERSION"
rm -rf "$WORK/mirror.git" "$WORK/verify.git"

echo "==> Building Flayr Chop $VERSION (universal)"
CMAKE="$(command -v cmake)" || die "cmake not found"
# FetchContent's git (the JUCE download) runs sealed too: no global/system config,
# templates, hooks, fsmonitor or attributes file.
BUILD_ENV=(env -i "PATH=$(dirname "$CMAKE"):/usr/bin:/bin:/usr/sbin:/sbin" "HOME=$HOME" "TMPDIR=${TMPDIR:-/tmp}"
           GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TEMPLATE_DIR="$EMPTY" GIT_ATTR_NOSYSTEM=1
           GIT_TERMINAL_PROMPT=0 GIT_CONFIG_COUNT=3
           GIT_CONFIG_KEY_0=core.hooksPath GIT_CONFIG_VALUE_0=/dev/null
           GIT_CONFIG_KEY_1=core.fsmonitor GIT_CONFIG_VALUE_1=false
           GIT_CONFIG_KEY_2=core.attributesFile GIT_CONFIG_VALUE_2=/dev/null)
"${BUILD_ENV[@]}" "$CMAKE" -S "$SRC" -B "$BUILD" -G Xcode \
    -DFLAYR_CHOP_COPY_AFTER_BUILD=OFF -DFLAYR_CHOP_TEST_HOOKS=OFF -DFLAYR_CHOP_BUILD_TESTS=OFF \
    -DFLAYR_CHOP_BUILD_TOOLS=OFF -DFLAYR_CHOP_BUILD_FUZZERS=OFF >/dev/null
"${BUILD_ENV[@]}" "$CMAKE" --build "$BUILD" --config Release --target FlayrChop_AU FlayrChop_VST3 -- -quiet

ART="$BUILD/FlayrChop_artefacts/Release"
echo "==> Checking the JUCE patches"
JUCE_SRC="$(sed -n 's/^juce_SOURCE_DIR:STATIC=//p' "$BUILD/CMakeCache.txt")"
[[ "$JUCE_SRC" == "$BUILD/_deps/juce-src" ]] || die "JUCE came from $JUCE_SRC, not from the pinned download"
patches="$(cd "$SRC" && ls patches/*.patch | sed "s|^|$SRC/|" | paste -sd'|' -)"
(cd "$JUCE_SRC" && "${BUILD_ENV[@]}" "$CMAKE" "-DPATCHES=$patches" -DCHECK_ONLY=ON -P "$SRC/cmake/ApplyPatch.cmake")
STAGE="$(mktemp -d)/Flayr Chop"
mkdir -p "$STAGE" "$DIST"
# Copies never follow a symbolic link: what's signed is exactly what the build produced.
for item in "$ART/AU/Flayr Chop.component" "$ART/VST3/Flayr Chop.vst3"; do
    [[ -d "$item" && ! -L "$item" ]] || die "$item is missing or a symbolic link"
done
cp -RP "$ART/AU/Flayr Chop.component" "$ART/VST3/Flayr Chop.vst3" "$STAGE/"

echo "==> Signing"
for bundle in "$STAGE/Flayr Chop.component" "$STAGE/Flayr Chop.vst3"; do
    codesign --force --options runtime --timestamp --sign "$IDENTITY" "$bundle"
    codesign --verify --strict --verbose=1 "$bundle"
done

echo "==> Notarizing the plug-ins"
ZIP="$(mktemp -d)/plugins.zip"
ditto -c -k --keepParent "$STAGE" "$ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$STAGE/Flayr Chop.component"
xcrun stapler staple "$STAGE/Flayr Chop.vst3"

echo "==> Building the DMG"
ln -s "/Library/Audio/Plug-Ins/Components" "$STAGE/Drag AU here (Logic, GarageBand)"
ln -s "/Library/Audio/Plug-Ins/VST3" "$STAGE/Drag VST3 here (Ableton, FL, Cubase, Reaper...)"
INSTALL_TXT="$SRC/packaging/macos/Install.txt"
[[ -f "$INSTALL_TXT" && ! -L "$INSTALL_TXT" ]] || die "Install.txt is missing or a symlink"
cp -P "$INSTALL_TXT" "$STAGE/Install.txt"
rm -f "$DMG"
hdiutil create -volname "Flayr Chop $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

echo "==> Notarizing the DMG"
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$DMG"
spctl --assess --type open --context context:primary-signature --verbose "$DMG"

echo "==> Done: $DMG"

if [[ "${1:-}" == "--upload" ]]; then
    REPO=FlayrLabs/flayr-chop
    # Only ever add to a draft: replacing files on a published release would swap what
    # people already downloaded and verified.
    [[ "$(gh release view "$TAG" --repo "$REPO" --json isDraft --jq .isDraft)" == "true" ]] \
        || die "release $TAG is already published (or missing); refusing to change its files"
    gh release upload "$TAG" "$DMG" --clobber --repo "$REPO"
    echo "==> Uploaded to release $TAG"

    # Checksums only for files we can vouch for: the DMG we just built and notarized
    # (byte-identical to the local copy), and CI builds whose provenance attestation
    # verifies. Anything else on the draft stops the script.
    sums_dir="$(mktemp -d)"
    gh release download "$TAG" --repo "$REPO" --dir "$sums_dir" --pattern 'FlayrChop-*'
    for asset in "$sums_dir"/FlayrChop-*; do
        name="$(basename "$asset")"
        if [[ "$name" == "$(basename "$DMG")" ]]; then
            cmp -s "$asset" "$DMG" || die "$name on the release differs from the DMG just built"
        else
            gh attestation verify "$asset" --repo "$REPO" >/dev/null || die "$name has no valid build provenance attestation"
        fi
        echo "    verified $name"
    done
    (cd "$sums_dir" && shasum -a 256 FlayrChop-* > SHA256SUMS.txt && cat SHA256SUMS.txt)
    gh release upload "$TAG" "$sums_dir/SHA256SUMS.txt" --clobber --repo "$REPO"
    rm -rf "$sums_dir"
    echo "==> Uploaded SHA256SUMS.txt"
fi
