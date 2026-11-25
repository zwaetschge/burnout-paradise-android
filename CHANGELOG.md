# Changelog

## 0.1.0 — 2026-11-25
First build-it-yourself release.
- Static recompilation of `BurnoutParadise.exe` (Steam 1.1.0.0): 36,626 functions from `.text`,
  function list in `seeds/`, pcrecomp with the patches in `patches/`.
- Win32 runtime with a fair single-guest-CPU scheduler, Direct3D 9 on DXVK/Vulkan, DirectSound on
  AAudio, the gamepad through the game's own XInput path, offline Winsock.
- Docker build in one command (`./build.sh`): lift, a 45-minute headless capture drive (run-time code
  from the sound bundles, shaders compiled with the game's own `d3dx9_37.dll` under Wine), ARM64 build,
  signed APK with 16 KiB alignment.
- Device scripts: copy the game files and write `port.env`, top up missing shaders
  (`scripts/shader-topup.sh`) and missing run-time code (`scripts/code-topup.sh`).
- Optional converter for the 41 extra vehicles of *Burnout Paradise Remastered* (overlay with a
  "Cop Cars" tab, junkyard pictures and texts).
- Tested on the AYN Odin 3: menus and free roam, 56–60 fps driving (60 Hz mode), 92–105 fps in the
  120 Hz mode.
