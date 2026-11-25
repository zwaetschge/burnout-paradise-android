# Project: burnout-paradise-android

Static recompilation of Burnout Paradise: The Ultimate Box (PC, Steam `BurnoutParadise.exe` 1.1.0.0)
for Android arm64. x86 `.text` → C (pcrecomp `generate.py` via `tools/static_full_lift.py`, plus
`tools/static_trampoline_pass.py` and `tools/port_patches.py`) → NDK clang → `libburnout_static.so`,
own Win32 runtime, Direct3D 9 on DXVK, AAudio. No Wine/Box64/emulator on the device.

## Source and legal boundary
- Supported EXE: SHA-256 fa55ff4d5b0f0f1e7a3dd861ec765d1d81c51554b588ae9872a3f923686f41be. The
  `.bind` Steam wrapper is skipped by starting at the OEP 0x9AA04B (`PORT_OEP` in `port.env`); never
  add decryption or license code.
- Never commit game files, `work/`, `out/`, `mods/`, lifted C, the dynamic-code pack, shader caches,
  `.so` or APKs. Run `python3 scripts/audit-clean-tree.py` before every commit.

## Pipeline (builder image `docker/Dockerfile.builder`, stages in `scripts/build-inside-docker.sh`)
- `./build.sh <game folder>` = `scripts/build-apk.sh` → container stages `lift`, `capture`, `lib`,
  `apk`; single stages with `--stage`. Work files in `work/` (gen, hot, dyn, shader-cache, android-lib).
- Capture: host build of the same runtime (zig, x86-64) + lavapipe + headless DXVK, hot loader
  (`PORT_HOT=1`, `tools/static_hot_helper.py`) lifts code the game loads at run time;
  `tools/static_dyn_pack.py` packs it for the static image. Shaders: `tools/d3dxcc` under Wine with the
  game's own `d3dx9_37.dll`.
- Device misses: `scripts/shader-topup.sh` (`.dxcq` → `.dxcr`), `scripts/code-topup.sh`
  (`dyn-window-<va>-<base>.bin` → stage `codetopup` → lib + apk).

## Runtime rules
- The guest has exactly one CPU: guest threads run under a fair single-CPU scheduler
  (`runtime/static_threads.c`); bridges release it only when they really block.
- Lifted code uses `RECOMP_LOCAL_REGS`, one CPU state object `g_cpu` (ARM64 register x28,
  `-ffixed-x28`) and a fixed guest base; do not add per-register thread-locals.
- Unknown APIs or instructions stop with a named trap instead of returning invented values.
- Diagnostics: `files/port-runtime.log` on the device; `PORT_TRACE_API=1`, `PORT_TRACE_INPUT=1`,
  `PORT_PROF=<start>:<seconds>`. `android/jni/port_cli.c` runs the image from an adb shell up to
  Direct3D without a window.
