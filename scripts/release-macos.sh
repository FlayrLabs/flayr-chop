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
VERSION="$(sed -n 's/^project(FlayrChop VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
BUILD="$ROOT/build-release-macos"
DIST="$ROOT/dist"
DMG="$DIST/FlayrChop-$VERSION-macOS.dmg"

echo "==> Building Flayr Chop $VERSION (universal)"
cmake -S "$ROOT" -B "$BUILD" -G Xcode -DFLAYR_CHOP_COPY_AFTER_BUILD=OFF -DFLAYR_CHOP_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" --config Release --target FlayrChop_AU FlayrChop_VST3 -- -quiet

ART="$BUILD/FlayrChop_artefacts/Release"
STAGE="$(mktemp -d)/Flayr Chop"
mkdir -p "$STAGE" "$DIST"
cp -R "$ART/AU/Flayr Chop.component" "$ART/VST3/Flayr Chop.vst3" "$STAGE/"

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
cp "$ROOT/packaging/macos/Install.txt" "$STAGE/Install.txt"
rm -f "$DMG"
hdiutil create -volname "Flayr Chop $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

echo "==> Notarizing the DMG"
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$DMG"
spctl --assess --type open --context context:primary-signature --verbose "$DMG"

echo "==> Done: $DMG"

if [[ "${1:-}" == "--upload" ]]; then
    gh release upload "v$VERSION" "$DMG" --clobber --repo FlayrLabs/flayr-chop
    echo "==> Uploaded to release v$VERSION"

    # Checksums for every file on the release, so users can verify their download.
    sums_dir="$(mktemp -d)"
    gh release download "v$VERSION" --repo FlayrLabs/flayr-chop --dir "$sums_dir" --pattern 'FlayrChop-*'
    (cd "$sums_dir" && shasum -a 256 FlayrChop-* > SHA256SUMS.txt && cat SHA256SUMS.txt)
    gh release upload "v$VERSION" "$sums_dir/SHA256SUMS.txt" --clobber --repo FlayrLabs/flayr-chop
    rm -rf "$sums_dir"
    echo "==> Uploaded SHA256SUMS.txt"
fi
