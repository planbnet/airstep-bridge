#!/bin/sh
# Builds AIRSTEP Bridge.app into build/.
# Pass --install to also copy it to /Applications.
set -e

cd "$(dirname "$0")"
APP="build/AIRSTEP Bridge.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"

swiftc -parse-as-library -O Sources/*.swift -o "$APP/Contents/MacOS/SparkBridge"
cp Resources/Info.plist "$APP/Contents/Info.plist"
codesign -s - -f --deep "$APP"

echo "built: $APP"

if [ "$1" = "--install" ]; then
    pkill -f "AIRSTEP Bridge.app/Contents/MacOS/SparkBridge" 2>/dev/null || true
    rm -rf "/Applications/AIRSTEP Bridge.app"
    cp -R "$APP" "/Applications/AIRSTEP Bridge.app"
    codesign -s - -f --deep "/Applications/AIRSTEP Bridge.app"
    echo "installed: /Applications/AIRSTEP Bridge.app"
fi
