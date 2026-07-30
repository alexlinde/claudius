# Claudius (macOS menu bar companion)

Native SwiftUI replacement for `companion/claudius.py`.

## Requirements

- macOS 14+
- [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`)
- Xcode 15+
- `claude` CLI on PATH (for session polling)

## Build & run

```bash
cd macos/Claudius
xcodegen generate
open Claudius.xcodeproj
```

Or from the CLI:

```bash
xcodegen generate
xcodebuild -scheme Claudius -configuration Release -destination 'platform=macOS' build
```

## Release DMG

Builds a signed `Claudius.dmg` (Developer ID + hardened runtime), suitable for drag-to-Applications install:

```bash
cd macos/Claudius
./release.sh --skip-notarize          # local / CI without Apple notarization
./release.sh                          # also notarize + staple (needs notarytool profile)
./release.sh --install                # also copy to ~/Applications and launch
```

One-time notarization credentials (reuse another app’s profile if you like):

```bash
xcrun notarytool store-credentials Claudius
# or: ./release.sh --notary-profile LambdaMonitor
```

Output: `macos/Claudius/.build/Claudius.dmg`

## Tests

```bash
xcodebuild -scheme Claudius -destination 'platform=macOS' test
```

The app is menu-bar only (`LSUIElement`). The mDNS name defaults to this Mac’s hostname (change in Preferences if needed). Optional secret must match the screen config.
