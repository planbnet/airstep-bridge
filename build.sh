#!/bin/sh
# Generates the Xcode project and builds.
#
#   ./build.sh                     build the Mac app
#   ./build.sh --install           build it and copy to /Applications
#   ./build.sh --ios               build for a connected iPhone/iPad
#
# Device builds need a signing team. Either select one once in Xcode, or set it
# in the environment:
#
#   DEVELOPMENT_TEAM=ABCDE12345 ./build.sh --ios
set -e

cd "$(dirname "$0")"

command -v xcodegen >/dev/null || { echo "xcodegen missing: brew install xcodegen"; exit 1; }
xcodegen generate

TEAM_ARG=""
[ -n "$DEVELOPMENT_TEAM" ] && TEAM_ARG="DEVELOPMENT_TEAM=$DEVELOPMENT_TEAM"

if [ "$1" = "--ios" ]; then
    xcodebuild -project AirstepBridge.xcodeproj \
               -scheme AirstepBridge-iOS \
               -configuration Release \
               -destination 'generic/platform=iOS' \
               -derivedDataPath build/dd \
               $TEAM_ARG \
               build
    echo "built: build/dd/Build/Products/Release-iphoneos/AIRSTEP Bridge.app"
    echo "install it with Xcode (Product > Run) or your usual deploy tool."
    exit 0
fi

xcodebuild -project AirstepBridge.xcodeproj \
           -scheme AirstepBridge-macOS \
           -configuration Release \
           -destination 'platform=macOS' \
           -derivedDataPath build/dd \
           $TEAM_ARG \
           build

APP="build/dd/Build/Products/Release/AIRSTEP Bridge.app"
echo "built: $APP"

if [ "$1" = "--install" ]; then
    pkill -f "AIRSTEP Bridge.app/Contents/MacOS" 2>/dev/null || true
    rm -rf "/Applications/AIRSTEP Bridge.app"
    cp -R "$APP" "/Applications/AIRSTEP Bridge.app"
    echo "installed: /Applications/AIRSTEP Bridge.app"
fi
