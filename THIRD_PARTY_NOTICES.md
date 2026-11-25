# Third-Party Notices & Legal Disclaimer

## Game files and binaries (not included)

This repository contains **no** copyrighted files from *Burnout Paradise* or *Burnout Paradise Remastered*
(Criterion Games / Electronic Arts): no game executables, DLLs, bundles, textures, sounds, videos,
shaders, saves, language files, and no code translated from the game.

To build and run the Android application you must supply your own legally owned copy of
**Burnout Paradise: The Ultimate Box** (Steam, `BurnoutParadise.exe` version `1.1.0.0`). Everything the
build produces from it stays on your machine (`work/`, `out/`) and on your device. **Do not upload or
redistribute built APKs, `libburnout_static.so`, lifted C sources, the dynamic-code pack, the shader
cache or converted vehicle files** — they are derived from your copy of the game.

The optional Remastered-vehicles tool (`tools/remastered_cars/`) reads a copy of *Burnout Paradise
Remastered* that you own and writes converted files into a local overlay folder. Those files are
derived from the Remastered game data and must not be redistributed either.

The port runs the game's own code starting at its program entry point. It does not decrypt anything,
does not contain any store, license or online-service component, and does not create or fake licenses
or DLC ownership.

---

## Open-source components & attribution

### 1. pcrecomp (static x86 recompiler)
- **Usage:** `tools/static_full_lift.py` drives the upstream lifter (`tools/lift/generate.py`) and the
  generated code uses its `runtime/recomp32` headers. Fetched at build time from
  <https://github.com/sp00nznet/pcrecomp> (commit `ef3f0da6e08a8eb7a3f869273f3df071ca8d7373`) and patched
  with `patches/pcrecomp-*.patch`.
- **License:** MIT.

### 2. DXVK (Direct3D 9 over Vulkan)
- **Usage:** `libdxvk_d3d9.so` on Android and a headless host build for the capture step. Fetched at
  build time from <https://github.com/doitsujin/dxvk> (tag `v3.1.1`, commit
  `b1a1c99ab52b687cf950d62c88bc2fa316b41663`) and patched with `patches/dxvk-android-wsi-and-build.patch`
  (Android/headless WSI, build fixes).
- **License:** zlib/libpng License.

### 3. Wine (build host only)
- **Usage:** runs Microsoft's `d3dx9_37.dll` from the game's own DirectX redistributable to compile the
  game's shaders on the build host; Wine's `c_1252.nls` / `sortdefault.nls` tables are copied to the
  device for the code-page functions. Installed from the Debian package archive inside the Docker image.
- **License:** LGPL-2.1-or-later (<https://www.winehq.org/license>).

### 4. Mesa lavapipe (build host only)
- **Usage:** software Vulkan driver for the headless capture run. Debian package `mesa-vulkan-drivers`.
- **License:** MIT.

### 5. Build-time tooling (not bundled)
- **Android NDK r27 / SDK build-tools 36** — Apache-2.0 / Android SDK License
- **Zig** (MIT) via the `ziglang` Python package, **Meson** (Apache-2.0), **Ninja** (Apache-2.0)
- **Capstone** (BSD-3-Clause), **pefile** (MIT), **Unicorn Engine** (GPL-2.0, used by the upstream
  lifter's test tooling only; never linked into anything that runs on the device)
- **NumPy** (BSD-3-Clause), **Pillow** (HPND), **texture2ddecoder** (MIT), **etcpak** (BSD-3-Clause) —
  optional, Remastered-vehicles tool only

## Trademarks

*Burnout*, *Burnout Paradise* and *Criterion* are trademarks of Electronic Arts Inc. This project is an
independent, unofficial compatibility and recompilation tool. It is not affiliated with, endorsed or
sponsored by Electronic Arts, Criterion Games, or Valve.
