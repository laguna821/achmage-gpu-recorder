# Achmage GPU Recorder (experimental)

A native hotkey display recorder inspired by dec05eba's Linux GPU Screen Recorder.
This is an independent project, not an official upstream Windows/macOS port.
GPL-3.0-only. See LICENSE and UPSTREAM.md.

## Behavior

- Windows: Win + Left Alt + R starts/stops the display under the mouse.
- macOS: Cmd + Option + R (experimental, requires Screen Recording permission).
- Silent video with cursor, H.264 MP4,30fps, at most1920×1080 preserving aspect ratio.
- Saved locally in your actual Downloads folder as Video_DATE_TIME.mp4.
- No uploads, AI calls, subscriptions, replay buffer or automatic video deletion.
- The hotkey receiver stays idle; the recording worker exits after finalizing.
- Hardware encoder required. A software encoder is never silently substituted.

## Windows

Build using MSVC x64 C++ Build Tools:

```powershell
./windows/setup-sdk.ps1
./windows/build.ps1
./build/windows/AchmageRecorder.exe inspect
```

Commands: install, uninstall, toggle, stop, status, exit, inspect, verify FILE.
Install.cmd installs the executable for the current user and a Startup shortcut.
Uninstall.cmd stops the receiver normally and removes only its executable/shortcut.
Recordings remain. No administrator rights required for this application.

The executable is not signed. No production release is claimed until recorded
runtime verification is published in VALIDATION.md. Conflicting Game Bar hotkeys
must be disabled/removed before using Win+Alt+R.

## macOS (experimental)

```sh
sh macos/build.sh
```

Requires macOS13+ and Xcode Command Line Tools. The build creates a Universal
Intel/Apple Silicon app and a drag-to-Applications DMG.
Ad-hoc signed only, not Developer-ID signed/notarized. Local Mac screen-capture,
timing, wake/sleep and repeated-exit tests are still required. Compilation is not
equivalent to runtime verification. The macOS backend is not a finished release.

## Platform/runtime limits

Protected content, secure desktops and hardware-encoding support vary by driver.
Display geometry change stops recording. Failed/incomplete files retain a
.partial.mp4 suffix. A complete MP4 rename happens only after finalization.
The2GiB free-space floor is a safety margin, not a reservation.

We do not ship the SDK, source checkout, personal videos or test recordings in
the application package. Measurements and known limitations live in VALIDATION.md.
