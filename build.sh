#!/bin/sh
# Generates the Xcode project and builds the macOS app.
# Pass --install to also copy it to /Applications.
#
# For iPhone/iPad: run ./build.sh (or xcodegen generate), open
# AirstepBridge.xcodeproj, pick the AirstepBridge-iOS scheme and your device.
set -e

cd "$(dirname "$0")"

command -v xcodegen >/dev/null || { echo "xcodegen missing: brew install xcodegen"; exit 1; }
xcodegen generate

xcodebuild -project AirstepBridge.xcodeproj \
           -scheme AirstepBridge-macOS \
           -configuration Release \
           -destination 'platform=macOS' \
           -derivedDataPath build/dd \
           build

APP="build/dd/Build/Products/Release/AIRSTEP Bridge.app"
echo "built: $APP"

if [ "$1" = "--install" ]; then
    pkill -f "AIRSTEP Bridge.app/Contents/MacOS" 2>/dev/null || true
    rm -rf "/Applications/AIRSTEP Bridge.app"
    cp -R "$APP" "/Applications/AIRSTEP Bridge.app"
    echo "installed: /Applications/AIRSTEP Bridge.app"
fi
