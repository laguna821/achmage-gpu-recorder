# Validation status — 2026-10-14

Windows11Home23H2 build22631, MSVC14.29, SDK26100.

- Native executable builds successfully (about382KiB initial build).
- Hardware encoder enumeration:3. Windows Graphics Capture supported.
- First capture initialization failed with E_INVALIDARG at MFCreateSinkWriterFromURL.
- Retest uses an explicit MPEG4 byte stream rather than automatic URL inference.
- Actual capture/playback,10min/60min sessions,10 cycles, idle RAM/CPU and hotkey tests: pending.
- macOS source: experimental, not built or runtime-tested on this Windows machine.
- Game Bar has not yet been replaced or removed.

No personal test video is published.
