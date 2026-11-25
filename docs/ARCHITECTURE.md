# How the port works

## Ahead-of-time translation

`tools/static_full_lift.py` drives the pcrecomp lifter over the `.text` section of `BurnoutParadise.exe`.
Every function becomes a C function that operates on a guest CPU state (`g_cpu`, kept in register `x28`
on ARM64) and a flat 32-bit guest address space at a fixed base. Function discovery starts from the
seed list in `seeds/` (entry points only), so the same executable always yields the same C sources.

`tools/static_trampoline_pass.py` turns direct calls between lifted functions into native calls, and
`tools/port_patches.py` inserts a few small, documented hooks into the generated C (simulation rate,
the offline "connection problem" dialog, the Remastered-vehicles overlay). The executable's data
sections are mapped at run time from `<game root>/BurnoutPR.exe`; only `.text` is translated.

Some game code is not in the executable: the sound bundles carry code that the game loads at run time.
On the build host a hot loader lifts it on the fly during the capture run, and
`tools/static_dyn_pack.py` packs everything that was seen into one translation unit for the device.
On the device, an unknown block is a named stop.

## Runtime

`runtime/` implements the Win32 surface the game uses: files (mapped to the Android game folder),
threads on a fair scheduler for exactly one guest CPU (`static_threads.c`), heaps, TLS/FLS, critical
sections and other synchronisation, registry (a small read-only table), locale and code pages
(Wine's NLS tables), timers, Winsock (offline), DirectInput/XInput/WMI so the game sees a native pad,
DirectSound on AAudio, and Direct3D 9 forwarded to DXVK on Vulkan (`d3d9_thunk.cpp`).

The game compiles its HLSL shaders at run time with `D3DXCompileShader`. The device has no compiler,
so `d3dx_thunk.cpp` serves results from a cache keyed by the request. The cache is produced on the
build host by the game's own `d3dx9_37.dll` running under Wine (`tools/d3dxcc`), and topped up later
from requests the device leaves behind (`scripts/shader-topup.sh`).

## Android

`android/jni/port_main.c` is a NativeActivity: it loads `libburnout_static.so`, hands it the window,
audio and input, and runs the game's entry point. `port.env` in the game folder sets optional switches.
