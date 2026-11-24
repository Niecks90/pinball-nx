# Changelog

All notable changes to Pinball NX are documented here.

## [0.1.1] - 2026-11-24

### Fixed
- Fixed instant crash on Nintendo Switch (Horizon OS) boot caused by allocating the ~585 KB `Pinball` emulator state on the main thread stack; moved `Pinball` and large buffers to heap allocation with null checks and set `__nx_main_thread_stack_size` to 1 MiB.
- Fixed read-only segment dynamic relocations (`TEXTREL`) in `Makefile` by compiling and linking with `-fPIE -mtp=soft` and removing `-Wl,-z,notext`.
- Fixed display initialization conflict between `consoleInit(NULL)` and `framebufferCreate()` on `nwindowGetDefault()`, enabled linear framebuffer mode (`framebufferMakeLinear()`), and fixed `0x00RRGGBB` to `RGBA8` channel order.
- Fixed touch-screen initialization and multi-touch handling (initialize once at startup, zero-initialize `HidTouchScreenState`, iterate over `state.count` instead of state struct count).
- Fixed missing-ROM screen text rendering using `font5x7.h` and clean `+` exit handling without dereferencing uninitialized emulator or ROM pointers.

### Added
- Early directory creation (`sdmc:/switch/pinball-nx`) and step-by-step startup diagnostics in `sdmc:/switch/pinball-nx/boot.log` with immediate flush/close per milestone.
- Automatic creation of default `sdmc:/switch/pinball-nx/config.ini` on first boot.
- 48000 Hz stereo `PcmFormat_Int16` audio output via `libnx` `audout` with safe fallback if audio initialization fails.
- Proper 90° CW and 270° CCW portrait/TATE screen rotation rendering and in-game `+` pause/settings overlay.

## [0.1.0] - 2026-11-23

- Added the initial Nintendo Switch port based on the upstream Pinball NS C11 runtime.
- Added native 16:9 landscape presentation for handheld and docked output.
- Added persisted landscape and portrait/TATE orientation modes at 90° and 270°.
- Added simultaneous multi-touch flipper, plunger, nudge, pause, and guide controls.
- Added modern libnx `PadState`/`padUpdate`, touch-screen input, lifecycle loop, and SD-card configuration handling.
- Added missing-ROM messaging, NACP metadata, a 256x256 icon, devkitA64 packaging, and GitHub Actions CI/release automation.

[0.1.1]: https://github.com/Niecks90/pinball-nx/releases/tag/v0.1.1
[0.1.0]: https://github.com/Niecks90/pinball-nx/releases/tag/v0.1.0
