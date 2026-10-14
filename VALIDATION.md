# Validation status — 2026-10-14

Windows11Home23H2 build22631, MSVC14.29, SDK26100.

- Native executable builds successfully (about382KiB initial build).
- Hardware encoder enumeration:3. Windows Graphics Capture supported.
- Initial sink creation failed with DISABLE_CONVERTERS enabled; removal of that option fixed it. NV12 conversion remains on GPU. The chosen encoder's hardware attribute is verified; observed NVIDIA H.264 Encoder MFT.
-10min engine test:612.433 seconds,18,373 encoded/decoded frames.60min engine test:3661.4 seconds,109,842 encoded/decoded frames. Every frame was decoded; workers returned normally.
- Long-test worker max working set:88,584,192bytes; max private allocation:191,930,368bytes. Private allocation is not resident RAM.
- Broker idle working set:11,173,888bytes; private allocation2,301,952bytes;30-second average CPU0% of one core. Windows may page process memory differently under other workloads.
- Native injected Win+LeftAlt+R start/stop and10 normal start/stop cycles passed. Windows retained the reserved hotkey after Game Bar removal; a specific-chord-only hook fallback was verified. No key data is recorded.
- Final supervision revision: an inherited synchronization-only parent handle ends the worker normally when the broker exits. Separate natural-parent-exit test finalized102 frames and fully decoded them. The final revision also passed another10 cycles and per-user installed capture/playback. Long-test binary preceded this supervision change; exact final binary was not run for another full hour.
- Game Bar/Xbox6 optional packages removed. Final per-user install and Startup shortcut verified. The recording worker exits; the tiny broker remains for the hotkey.
- Windows lock/sleep/physical monitor unplug, HDR color accuracy, physical-key handling with every keyboard layout, mobile reconnection and the next reboot remain native checks. Implemented handlers are not counted as executed tests.
- macOS Universal Intel/Apple Silicon app and DMG compile successfully in GitHub Actions. Screen capture permission, actual recording, static-frame end timing, RAM and wake/sleep require a real Mac test. Ad-hoc signed, not notarized.

No personal test video is published.
