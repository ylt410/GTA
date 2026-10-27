#!/bin/bash
# Engine-free UIKit regression checks. Pass a booted iOS simulator UDID.
set -euo pipefail
APP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEVICE="${1:?Usage: bash app/Tests/run_airplane_controls.sh BOOTED_SIMULATOR_UDID}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/airplane-controls.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
APP="$WORK/ControlsChecks.app"
mkdir "$APP"
cat "$APP_ROOT/App/TouchControlsView.swift" "$APP_ROOT/Tests/airplane_controls.swift" > "$WORK/Checks.swift"
xcrun swiftc -sdk "$(xcrun --sdk iphonesimulator --show-sdk-path)" \
  -target arm64-apple-ios16.0-simulator "$WORK/Checks.swift" "$APP_ROOT/App/VirtualPad.swift" \
  -framework UIKit -framework GameController -o "$APP/ControlsChecks"
/usr/bin/python3 - "$APP/Info.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'wb') as stream:
    plistlib.dump({
        'CFBundleIdentifier': 'io.github.playgta5ios.airplane-controls-tests',
        'CFBundleExecutable': 'ControlsChecks', 'CFBundleName': 'Controls Checks',
        'CFBundlePackageType': 'APPL', 'CFBundleVersion': '1',
        'CFBundleShortVersionString': '1', 'MinimumOSVersion': '16.0',
        'UILaunchScreen': {}, 'UISupportedInterfaceOrientations': [
            'UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
    }, stream)
PY
codesign --force --sign - "$APP"
xcrun simctl install "$DEVICE" "$APP"
xcrun simctl launch --console "$DEVICE" io.github.playgta5ios.airplane-controls-tests > "$WORK/results.log" 2>&1
cat "$WORK/results.log"
grep -q 'AIRPLANE CONTROL REGRESSION CHECKS PASSED' "$WORK/results.log"
