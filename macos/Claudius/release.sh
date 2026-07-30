#!/bin/bash
set -euo pipefail

# Build, sign, package, and notarize Claudius.app as a DMG.
# Pattern adapted from ~/aescape/LambdaMonitor/release.sh

cd "$(dirname "$0")"

APP_NAME="Claudius"
BUNDLE_ID="com.gmclaude.claudius"
IDENTITY="Developer ID Application: Alex Linde (TN7Z2D3D5R)"
NOTARY_PROFILE="Claudius"
VERSION="$(awk -F'"' '/MARKETING_VERSION:/ {print $2; exit}' project.yml)"
VERSION="${VERSION:-1.0.0}"

STAGING=".build/release-staging"
DERIVED="$STAGING/DerivedData"
APP_BUNDLE="$STAGING/$APP_NAME.app"
DMG_NAME="$APP_NAME.dmg"
DMG_TEMP="$STAGING/$APP_NAME-temp.dmg"
DMG_FINAL=".build/$DMG_NAME"
ENTITLEMENTS="Claudius/Claudius.entitlements"

SKIP_NOTARIZE=0
INSTALL_LOCAL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --notary-profile) NOTARY_PROFILE="$2"; shift 2 ;;
        --skip-notarize) SKIP_NOTARIZE=1; shift ;;
        --install) INSTALL_LOCAL=1; shift ;;
        --version) VERSION="$2"; shift 2 ;;
        -h|--help)
            cat <<EOF
Usage: ./release.sh [options]

Build a signed, installable Claudius.dmg (and optionally notarize it).

Options:
  --skip-notarize          Build + sign + DMG only (no notarytool)
  --notary-profile NAME    Keychain profile for notarytool (default: $NOTARY_PROFILE)
  --install                Also copy Claudius.app to ~/Applications and open it
  --version X.Y.Z          Override MARKETING_VERSION from project.yml
  -h, --help               Show this help

One-time notarization setup (same Apple ID as other apps is fine):
  xcrun notarytool store-credentials Claudius
EOF
            exit 0
            ;;
        *) echo "Unknown option: $1 (try --help)"; exit 1 ;;
    esac
done

echo "==> Claudius release ${VERSION} (arm64)"

# ── Generate Xcode project ─────────────────────────────────────────────────────

if ! command -v xcodegen >/dev/null 2>&1; then
    echo "error: xcodegen not found (brew install xcodegen)" >&2
    exit 1
fi

echo "==> Generating Xcode project..."
xcodegen generate

# ── Build (unsigned; we re-sign with Developer ID) ─────────────────────────────

echo "==> Building Release (arm64 only)..."
rm -rf "$STAGING"
mkdir -p "$STAGING"

set +e
xcodebuild \
    -scheme Claudius \
    -configuration Release \
    -destination 'platform=macOS,arch=arm64' \
    -derivedDataPath "$DERIVED" \
    ARCHS=arm64 \
    ONLY_ACTIVE_ARCH=YES \
    EXCLUDED_ARCHS=x86_64 \
    CODE_SIGN_IDENTITY="-" \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGNING_ALLOWED=NO \
    MARKETING_VERSION="$VERSION" \
    CURRENT_PROJECT_VERSION="$VERSION" \
    build \
    2>&1 | tee "$STAGING/xcodebuild.log" \
    | grep -E '^(error:|warning:|/\*|CompileSwift|Ld |CodeSign|\*\*)' || true
XC_STATUS=${PIPESTATUS[0]}
set -e
if [[ "$XC_STATUS" -ne 0 ]]; then
    echo "error: xcodebuild failed (exit ${XC_STATUS}); see ${STAGING}/xcodebuild.log" >&2
    exit "$XC_STATUS"
fi

APP_SRC=$(find "$DERIVED/Build/Products/Release" -maxdepth 1 -name "$APP_NAME.app" -print -quit)
if [[ -z "$APP_SRC" || ! -d "$APP_SRC" ]]; then
    echo "error: ${APP_NAME}.app not found under ${DERIVED}/Build/Products/Release" >&2
    exit 1
fi

echo "==> Assembling ${APP_NAME}.app..."
rm -rf "$APP_BUNDLE"
ditto "$APP_SRC" "$APP_BUNDLE"

# Keep Info.plist version in sync with the release we just built.
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" \
    "$APP_BUNDLE/Contents/Info.plist" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :CFBundleShortVersionString string $VERSION" \
        "$APP_BUNDLE/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" \
    "$APP_BUNDLE/Contents/Info.plist" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :CFBundleVersion string $VERSION" \
        "$APP_BUNDLE/Contents/Info.plist"

# ── Codesign (hardened runtime) ────────────────────────────────────────────────

echo "==> Signing with hardened runtime (${IDENTITY})..."
codesign --force --deep --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$IDENTITY" \
    "$APP_BUNDLE"

codesign --verify --deep --strict "$APP_BUNDLE"
echo "  Signature verified."

# ── Optional local install ─────────────────────────────────────────────────────

if [[ "$INSTALL_LOCAL" == "1" ]]; then
    DEST="${HOME}/Applications/${APP_NAME}.app"
    echo "==> Installing to ${DEST}..."
    mkdir -p "${HOME}/Applications"
    rm -rf "${DEST}"
    ditto "$APP_BUNDLE" "${DEST}"
    codesign --force --deep --options runtime \
        --entitlements "$ENTITLEMENTS" \
        --sign "$IDENTITY" \
        "${DEST}"
fi

# ── Create DMG ─────────────────────────────────────────────────────────────────

echo "==> Creating DMG..."
rm -f "$DMG_TEMP" "$DMG_FINAL"
mkdir -p .build

DMG_VOLUME="$APP_NAME"
DMG_SIZE=64  # MB

if [[ -d "/Volumes/$DMG_VOLUME" ]]; then
    hdiutil detach "/Volumes/$DMG_VOLUME" -quiet || true
fi

hdiutil create \
    -size "${DMG_SIZE}m" \
    -fs HFS+ \
    -volname "$DMG_VOLUME" \
    "$DMG_TEMP"

ATTACH_OUTPUT=$(hdiutil attach "$DMG_TEMP" -readwrite -noverify)
MOUNT_DIR=$(printf "%s\n" "$ATTACH_OUTPUT" | awk '/\/Volumes\// {print $NF}' | tail -1)
if [[ -z "$MOUNT_DIR" || ! -d "$MOUNT_DIR" ]]; then
    echo "Failed to find DMG mount point from hdiutil output:"
    echo "$ATTACH_OUTPUT"
    exit 1
fi

ditto "$APP_BUNDLE" "$MOUNT_DIR/$APP_NAME.app"
rm -f "$MOUNT_DIR/Applications"
ln -s /Applications "$MOUNT_DIR/Applications"

osascript <<APPLESCRIPT
tell application "Finder"
    tell disk "$DMG_VOLUME"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set bounds of container window to {400, 200, 900, 460}
        set theViewOptions to icon view options of container window
        set arrangement of theViewOptions to not arranged
        set icon size of theViewOptions to 80
        set position of item "$APP_NAME.app" of container window to {120, 130}
        set position of item "Applications" of container window to {380, 130}
        close
        open
        delay 1
        close
    end tell
end tell
APPLESCRIPT

sync
hdiutil detach "$MOUNT_DIR" -quiet

hdiutil convert "$DMG_TEMP" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -o "$DMG_FINAL"

rm -f "$DMG_TEMP"
echo "  Created: ${DMG_FINAL}"

# ── Notarize ───────────────────────────────────────────────────────────────────

if [[ "$SKIP_NOTARIZE" == "1" ]]; then
    echo "==> Skipping notarization (--skip-notarize)."
else
    echo "==> Notarizing (profile: ${NOTARY_PROFILE})..."
    echo "  (Set up credentials once with: xcrun notarytool store-credentials ${NOTARY_PROFILE})"
    xcrun notarytool submit "$DMG_FINAL" \
        --keychain-profile "$NOTARY_PROFILE" \
        --wait

    echo "==> Stapling notarization ticket..."
    xcrun stapler staple "$DMG_FINAL"
fi

# ── Done ───────────────────────────────────────────────────────────────────────

echo ""
echo "OK Release ready: ${DMG_FINAL}"
ls -lh "$DMG_FINAL"

if [[ "$INSTALL_LOCAL" == "1" ]]; then
    echo "  Installed: ~/Applications/${APP_NAME}.app"
    open "${HOME}/Applications/${APP_NAME}.app"
fi
