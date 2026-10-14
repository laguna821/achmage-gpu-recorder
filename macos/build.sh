#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
OUT="build/macos/Achmage Recorder.app"
mkdir -p "$OUT/Contents/MacOS"
xcrun swiftc -swift-version 5 -parse-as-library -O -target "$(uname -m)-apple-macosx13.0" \
  macos/Recorder.swift -o "$OUT/Contents/MacOS/AchmageRecorder" \
  -framework AppKit -framework Carbon -framework ScreenCaptureKit -framework AVFoundation -framework VideoToolbox
cp macos/Info.plist "$OUT/Contents/Info.plist"
codesign --force --sign - "$OUT"
mkdir -p dist
hdiutil create -volname "Achmage Recorder (experimental)" -srcfolder "$OUT" -ov -format UDZO dist/AchmageRecorder-macos-experimental.dmg
