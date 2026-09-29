#!/bin/zsh
# Builds an Apple Silicon (arm64), Developer ID-signed, notarized and stapled GradientRemap.plugin and
# packages it with LICENSE.txt into dist/GradientRemap-v<version>-macOS.zip.
#
# One-time setup (stores an app-specific password in the login keychain):
#   xcrun notarytool store-credentials AC_NOTARY_PROFILE \
#       --apple-id <apple-id-email> --team-id YAEAPD4M84
#
# Overridable: SIGN_IDENTITY, NOTARY_PROFILE.
set -euo pipefail

ROOT="${0:A:h:h}"
BUILD="$ROOT/build/release"
DIST="$ROOT/dist"
PLUGIN="$BUILD/GradientRemap.plugin"
SIGN_IDENTITY="${SIGN_IDENTITY:-Developer ID Application: Rob Payne (YAEAPD4M84)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-AC_NOTARY_PROFILE}"

MAJOR=$(awk '/#define MAJOR_VERSION/ {print $3}' "$ROOT/GradientRemap/src/plugin/GradientRemap.h")
MINOR=$(awk '/#define MINOR_VERSION/ {print $3}' "$ROOT/GradientRemap/src/plugin/GradientRemap.h")
VERSION="$MAJOR.$MINOR"
ZIP_NAME="GradientRemap-v$VERSION-macOS.zip"

echo "==> Building v$VERSION (arm64, clean)"
rm -rf "$BUILD"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DGRADIENT_REMAP_CODESIGN_IDENTITY="$SIGN_IDENTITY" >/dev/null
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure

echo "==> Verifying signature"
codesign --verify --strict --verbose=2 "$PLUGIN"

echo "==> Submitting for notarization (usually a few minutes)"
SUBMIT_ZIP="$BUILD/notarize-upload.zip"
ditto -c -k --keepParent "$PLUGIN" "$SUBMIT_ZIP"
xcrun notarytool submit "$SUBMIT_ZIP" --keychain-profile "$NOTARY_PROFILE" --wait

echo "==> Stapling ticket"
xcrun stapler staple "$PLUGIN"
xcrun stapler validate "$PLUGIN"
spctl -a -vv -t install "$PLUGIN"

echo "==> Packaging"
STAGE="$BUILD/stage"
rm -rf "$STAGE" && mkdir -p "$STAGE" "$DIST"
ditto "$PLUGIN" "$STAGE/GradientRemap.plugin"
cp "$ROOT/LICENSE.txt" "$STAGE/"
rm -f "$DIST/$ZIP_NAME"
(cd "$STAGE" && ditto -c -k --norsrc --noextattr --noqtn . "$DIST/$ZIP_NAME")

echo "==> Done: $DIST/$ZIP_NAME"
