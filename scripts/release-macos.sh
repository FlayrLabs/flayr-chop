#!/usr/bin/env bash
# Builds the macOS release (universal AU + VST3), signs it with a Developer ID,
# notarizes and staples it, and wraps it in a drag-to-install DMG.
#
#   scripts/release-macos.sh            # build dist/FlayrChop-<version>-macOS.dmg
#   scripts/release-macos.sh --upload   # ...and attach it to the GitHub release v<version>
#
# Needs a "Developer ID Application" identity in the keychain and a notarytool
# keychain profile (xcrun notarytool store-credentials <profile> ...).
set -euo pipefail

IDENTITY="${IDENTITY:-Developer ID Application: Flayr Labs LLC (NRNU83UJ68)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-flayr-notary}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Nothing from this shell may steer the build: CMake and FetchContent read overrides from the
# environment (a JUCE checkout elsewhere, a toolchain file, extra flags), and the build runs below
# under `env -i` with a minimal PATH and a scratch HOME anyway.
for var in $(env | grep -oE '^(FETCHCONTENT|CMAKE)_[A-Za-z0-9_]*' || true) CC CXX CFLAGS CXXFLAGS CPPFLAGS LDFLAGS \
           OBJC OBJCXX SDKROOT MACOSX_DEPLOYMENT_TARGET DEVELOPER_DIR; do
    if [[ -n "${!var+x}" ]]; then
        echo "error: $var is set; release builds take no build settings from the environment" >&2
        exit 1
    fi
done
CMAKE_BIN="$(command -v cmake)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/home"
REPO_URL="https://github.com/FlayrLabs/flayr-chop.git"

# Every git command runs sealed: no environment (GIT_TEMPLATE_DIR, GIT_DIR, ...), no system or
# global config (hooks, filters, insteadOf, templates), and hooks and fsmonitor off even if the
# repository's own config asks for them. Only plumbing that reads objects is run on the local
# repository: nothing here checks out, so no smudge/clean filter or hook ever runs.
git_sealed() {
    env -i "HOME=$WORK/home" "PATH=/usr/bin:/bin" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0 \
        /usr/bin/git -c core.hooksPath=/dev/null -c core.fsmonitor=false -c core.autocrlf=false "$@"
}

# Only ever sign exactly what a release tag holds: the commit HEAD's v* tag names, the same commit
# the tag names on GitHub, exported from the object database (no working tree, local edit or
# untracked file can reach the build) and checked against the tag's tree.
TAG="$(git_sealed -C "$ROOT" describe --tags --exact-match --match 'v*' HEAD 2>/dev/null || true)"
if [[ -z "$TAG" || ! "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: HEAD is not exactly at a vX.Y.Z release tag" >&2
    exit 1
fi
TAG_COMMIT="$(git_sealed -C "$ROOT" rev-parse --verify "$TAG^{commit}")"
TAG_TREE="$(git_sealed -C "$ROOT" rev-parse --verify "$TAG^{tree}")"
REMOTE_COMMIT="$(git_sealed ls-remote "$REPO_URL" "refs/tags/$TAG^{}" "refs/tags/$TAG" | awk -v t="refs/tags/$TAG" '$2 == t "^{}" { peeled = $1 } $2 == t { plain = $1 } END { print (peeled != "" ? peeled : plain) }')"
if [[ "$REMOTE_COMMIT" != "$TAG_COMMIT" ]]; then
    echo "error: $TAG is $TAG_COMMIT here but '${REMOTE_COMMIT:-missing}' on $REPO_URL" >&2
    exit 1
fi
VERSION="${TAG#v}"

SRC="$WORK/src"
mkdir -p "$SRC"
git_sealed -C "$ROOT" archive --format=tar "$TAG_COMMIT" | tar -x -C "$SRC" -f -
# The exported files must be exactly the tag's tree (index and objects in a scratch repository).
EXPORTED_TREE="$(GIT_DIR="$WORK/verify.git" GIT_WORK_TREE="$SRC" bash -c '
    env -i "HOME=$0" "PATH=/usr/bin:/bin" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null "GIT_DIR=$1" "GIT_WORK_TREE=$2" \
        /usr/bin/git -c core.hooksPath=/dev/null -c core.autocrlf=false init -q &&
    env -i "HOME=$0" "PATH=/usr/bin:/bin" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null "GIT_DIR=$1" "GIT_WORK_TREE=$2" \
        /usr/bin/git -c core.hooksPath=/dev/null -c core.autocrlf=false add -A -f &&
    env -i "HOME=$0" "PATH=/usr/bin:/bin" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null "GIT_DIR=$1" "GIT_WORK_TREE=$2" \
        /usr/bin/git -c core.hooksPath=/dev/null write-tree' "$WORK/home" "$WORK/verify.git" "$SRC")"
if [[ "$EXPORTED_TREE" != "$TAG_TREE" ]]; then
    echo "error: the exported sources ($EXPORTED_TREE) are not $TAG's tree ($TAG_TREE)" >&2
    exit 1
fi
if [[ "$(sed -n 's/^project(FlayrChop VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")" != "$VERSION" ]]; then
    echo "error: CMakeLists.txt version does not match tag $TAG" >&2
    exit 1
fi

BUILD="$WORK/build"
DIST="$ROOT/dist"
DMG="$DIST/FlayrChop-$VERSION-macOS.dmg"

echo "==> Building Flayr Chop $VERSION (universal) from the sources of $TAG"
clean_env=(env -i "HOME=$WORK/home" "PATH=$(dirname "$CMAKE_BIN"):/usr/bin:/bin:/usr/sbin:/sbin" "TMPDIR=$WORK"
           GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0) # FetchContent runs git too
"${clean_env[@]}" "$CMAKE_BIN" -S "$SRC" -B "$BUILD" -G Xcode \
    -DFLAYR_CHOP_COPY_AFTER_BUILD=OFF \
    -DFLAYR_CHOP_TEST_HOOKS=OFF \
    -DFLAYR_CHOP_BUILD_TESTS=OFF \
    -DFLAYR_CHOP_BUILD_TOOLS=OFF \
    -DFLAYR_CHOP_BUILD_FUZZERS=OFF >/dev/null
"${clean_env[@]}" "$CMAKE_BIN" --build "$BUILD" --config Release --target FlayrChop_AU FlayrChop_VST3 -- -quiet

ART="$BUILD/FlayrChop_artefacts/Release"
STAGE="$(mktemp -d)/Flayr Chop"
mkdir -p "$STAGE" "$DIST"
# Copies never follow a symbolic link: what's signed is exactly what the build produced.
for item in "$ART/AU/Flayr Chop.component" "$ART/VST3/Flayr Chop.vst3" "$SRC/packaging/macos/Install.txt"; do
    if [[ -L "$item" ]]; then
        echo "error: $item is a symbolic link" >&2
        exit 1
    fi
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
cp -P "$SRC/packaging/macos/Install.txt" "$STAGE/Install.txt"
rm -f "$DMG"
hdiutil create -volname "Flayr Chop $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

echo "==> Notarizing the DMG"
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$DMG"
spctl --assess --type open --context context:primary-signature --verbose "$DMG"

echo "==> Done: $DMG"

if [[ "${1:-}" == "--upload" ]]; then
    # Only ever add to a draft: a published release's files are what users already downloaded.
    if [[ "$(gh release view "v$VERSION" --repo FlayrLabs/flayr-chop --json isDraft --jq .isDraft)" != "true" ]]; then
        echo "error: release v$VERSION is not a draft; not changing a published release" >&2
        exit 1
    fi
    gh release upload "v$VERSION" "$DMG" --repo FlayrLabs/flayr-chop # fails if a DMG is already there
    echo "==> Uploaded to release v$VERSION"

    # Checksums for every file on the release, so users can verify their download.
    sums_dir="$(mktemp -d)"
    gh release download "v$VERSION" --repo FlayrLabs/flayr-chop --dir "$sums_dir" --pattern 'FlayrChop-*'
    (cd "$sums_dir" && shasum -a 256 FlayrChop-* > SHA256SUMS.txt && cat SHA256SUMS.txt)
    gh release upload "v$VERSION" "$sums_dir/SHA256SUMS.txt" --clobber --repo FlayrLabs/flayr-chop # the draft's list, now with the DMG
    rm -rf "$sums_dir"
    echo "==> Uploaded SHA256SUMS.txt"
fi
