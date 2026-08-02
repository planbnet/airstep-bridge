#!/bin/sh
# Renders the app icon and fills Resources/Assets.xcassets/AppIcon.appiconset.
# Run after changing Tools/makeicon.swift; the generated PNGs are checked in so a
# plain build does not need this step.
set -e

cd "$(dirname "$0")/.."
SET="Resources/Assets.xcassets/AppIcon.appiconset"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$SET" build
swiftc -O Tools/makeicon.swift -o "$TMP/makeicon"
"$TMP/makeicon" "$TMP/ios.png" "$TMP/macos.png"

rm -f "$SET"/*.png
cp "$TMP/ios.png" "$SET/icon-ios-1024.png"

# macOS grid: 16, 32, 128, 256, 512 at 1x and 2x
for px in 16 32 64 128 256 512 1024; do
    sips -Z $px "$TMP/macos.png" --out "$SET/icon-mac-$px.png" >/dev/null
done

cat > "$SET/Contents.json" <<'JSON'
{
  "images" : [
    { "idiom" : "universal", "platform" : "ios", "size" : "1024x1024", "filename" : "icon-ios-1024.png" },
    { "idiom" : "mac", "scale" : "1x", "size" : "16x16",   "filename" : "icon-mac-16.png" },
    { "idiom" : "mac", "scale" : "2x", "size" : "16x16",   "filename" : "icon-mac-32.png" },
    { "idiom" : "mac", "scale" : "1x", "size" : "32x32",   "filename" : "icon-mac-32.png" },
    { "idiom" : "mac", "scale" : "2x", "size" : "32x32",   "filename" : "icon-mac-64.png" },
    { "idiom" : "mac", "scale" : "1x", "size" : "128x128", "filename" : "icon-mac-128.png" },
    { "idiom" : "mac", "scale" : "2x", "size" : "128x128", "filename" : "icon-mac-256.png" },
    { "idiom" : "mac", "scale" : "1x", "size" : "256x256", "filename" : "icon-mac-256.png" },
    { "idiom" : "mac", "scale" : "2x", "size" : "256x256", "filename" : "icon-mac-512.png" },
    { "idiom" : "mac", "scale" : "1x", "size" : "512x512", "filename" : "icon-mac-512.png" },
    { "idiom" : "mac", "scale" : "2x", "size" : "512x512", "filename" : "icon-mac-1024.png" }
  ],
  "info" : { "author" : "xcode", "version" : 1 }
}
JSON

cat > Resources/Assets.xcassets/Contents.json <<'JSON'
{ "info" : { "author" : "xcode", "version" : 1 } }
JSON

echo "icons written to $SET"
