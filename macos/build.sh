#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
OUT="build/macos/Achmage Recorder.app"
mkdir -p "$OUT/Contents/MacOS"
for ARCH in arm64 x86_64; do
  xcrun swiftc -swift-version 5 -parse-as-library -O -target "$ARCH-apple-macosx13.0" \
    macos/Recorder.swift -o "build/macos/recorder-$ARCH" \
    -framework AppKit -framework Carbon -framework ScreenCaptureKit -framework AVFoundation -framework VideoToolbox
done
xcrun lipo -create build/macos/recorder-arm64 build/macos/recorder-x86_64 -output "$OUT/Contents/MacOS/AchmageRecorder"
cp macos/Info.plist "$OUT/Contents/Info.plist"
codesign --force --sign - "$OUT"
mkdir -p dist
STAGE="build/macos/dmg-root"
mkdir -p "$STAGE"
cp -R "$OUT" "$STAGE/"
cp LICENSE README.md "$STAGE/"
if [ ! -e "$STAGE/Applications" ] && [ ! -L "$STAGE/Applications" ]; then ln -s /Applications "$STAGE/Applications"; fi
hdiutil create -volname "Achmage Recorder (experimental)" -srcfolder "$STAGE" -ov -format UDZO dist/AchmageRecorder-macos-experimental.dmg
