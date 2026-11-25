// Direct3D 9: 32-bit guest COM <-> 64-bit DXVK. See d3d9_thunk.h.
//
// Rules of this file:
//   * every HRESULT, capability and pixel comes from DXVK; nothing is invented
//   * anything not understood is a named stop (stop("...")), never a fake S_OK
//   * guest memory is the flat mapping: host address = g_base + guest address
#include "d3d9_thunk.h"

#include <d3d9.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kVtableBase = 0x6D100000u;
constexpr uint32_t kVtableStride = 0x400u;          // 256 slots per interface
constexpr uint32_t kObjectBase = 0x6D200000u;
constexpr uint32_t kObjectLimit = 0x6E000000u;
constexpr uint32_t kObjectSize = 16u;
constexpr uint32_t kMaxObjects = (kObjectLimit - kObjectBase) / kObjectSize;
constexpr uint32_t kArenaBase = 0x61000000u, kArenaLimit = 0x6D000000u;
constexpr uint32_t kOverflowBase = 0x80000000u, kOverflowLimit = 0xC0000000u;
constexpr uint32_t kObjectMagic = 0x39443344u;      // "D3D9"

uint8_t *g_base;
int g_trace;
std::atomic<uint64_t> g_calls{0};
thread_local const char *t_stop = "";

inline void *P(uint32_t va) { return va ? g_base + va : nullptr; }
inline uint32_t R32(uint32_t va) { uint32_t v; memcpy(&v, g_base + va, 4); return v; }
inline void W32(uint32_t va, uint32_t v) { memcpy(g_base + va, &v, 4); }
inline float f32(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
int stop(const char *why) { t_stop = why; return -1; }
/* An object argument the thunk does not know: report the guest value (diagnostics). */
int stop_object(uint32_t guest) {
    fprintf(stderr, "D3D9T unknown object argument 0x%08X\n", guest);
    return stop("unknown-object-argument");
}

#define LOGF(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

// ---------------------------------------------------------------- lock arena
// Guest memory handed to the game by Lock/LockRect/LockBox.
struct Arena {
    std::mutex lock;
    std::map<uint32_t, uint32_t> free_blocks;       // address -> size
    std::unordered_map<uint32_t, uint32_t> used;    // address -> size
    uint64_t in_use = 0, peak = 0;
    bool overflow_logged = false;
} g_arena;

uint32_t ga_alloc(uint32_t size) {
    size = (size + 63u) & ~63u;
    if (!size) size = 64;
    std::lock_guard<std::mutex> guard(g_arena.lock);
    for (auto it = g_arena.free_blocks.begin(); it != g_arena.free_blocks.end(); ++it) {
        if (it->second < size) continue;
        uint32_t address = it->first, rest = it->second - size;
        if (address >= kOverflowBase && !g_arena.overflow_logged) {
            g_arena.overflow_logged = true;
            LOGF("D3D9T lock arena below 2 GiB exhausted (%llu MiB in use): using 0x80000000+", (unsigned long long)(g_arena.in_use >> 20));
        }
        g_arena.free_blocks.erase(it);
        if (rest) g_arena.free_blocks[address + size] = rest;
        g_arena.used[address] = size;
        g_arena.in_use += size;
        if (g_arena.in_use > g_arena.peak) g_arena.peak = g_arena.in_use;
        return address;
    }
    return 0;
}

void ga_free(uint32_t address) {
    if (!address) return;
    std::lock_guard<std::mutex> guard(g_arena.lock);
    auto found = g_arena.used.find(address);
    if (found == g_arena.used.end()) return;
    uint32_t size = found->second;
    g_arena.used.erase(found);
    g_arena.in_use -= size;
    if (size >= (1u << 20)) {                       // give big blocks back to the kernel
        uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
        uintptr_t lo = ((uintptr_t)(g_base + address) + page - 1) & ~(page - 1);
        uintptr_t hi = (uintptr_t)(g_base + address + size) & ~(page - 1);
        if (hi > lo) madvise((void *)lo, hi - lo, MADV_DONTNEED);
    }
    auto next = g_arena.free_blocks.lower_bound(address);
    if (next != g_arena.free_blocks.end() && address + size == next->first) {
        size += next->second;
        next = g_arena.free_blocks.erase(next);
    }
    if (next != g_arena.free_blocks.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == address) { prev->second += size; return; }
    }
    g_arena.free_blocks[address] = size;
}

// ------------------------------------------------------------------- objects
struct Method { const char *name; int (*fn)(const uint32_t *a, uint32_t *r); uint32_t argc; };
struct Interface { const char *name; GUID iid; const Method *methods; uint32_t count; };

struct BufferLock { uint32_t offset, size; uint8_t *host; bool write; };
struct Object {
    IUnknown *host = nullptr;
    uint16_t iface = 0;
    uint32_t shadow = 0, shadow_size = 0;           // vertex/index buffer: guest copy
    bool dynamic = false;                           // vertex/index buffer: D3DUSAGE_DYNAMIC
    std::vector<BufferLock> locks;
};

std::mutex g_lock;                                  // objects, locks, texture locks
Object *g_objects[kMaxObjects];
std::atomic<uint32_t> g_object_count{0};
std::unordered_map<void *, uint32_t> g_by_host;     // host object -> index

struct TextureLock {
    uint32_t guest; uint8_t *host;
    uint32_t pitch, rows, row_bytes, slice_pitch, slices;
    bool write;
};
std::unordered_map<uint64_t, TextureLock> g_texture_locks;   // (object, subresource)

inline Object *object_of(uint32_t va) {
    if (va < kObjectBase || va >= kObjectLimit || (va & (kObjectSize - 1))) return nullptr;
    uint32_t index = (va - kObjectBase) / kObjectSize;
    if (index >= g_object_count.load(std::memory_order_acquire)) return nullptr;
    return g_objects[index];
}
inline IUnknown *host_of(uint32_t va) {
    Object *object = object_of(va);
    return object ? object->host : nullptr;
}

uint32_t wrap_typed(IUnknown *host, int iface);

// HWND: the game has one window; every guest HWND is the native window.
void *g_native_window;
int g_window_width, g_window_height;
uint32_t g_guest_hwnd;
inline HWND hwnd_of(uint32_t guest) {
    if (!guest) return nullptr;
    if (!g_guest_hwnd) g_guest_hwnd = guest;
    return (HWND)g_native_window;
}

// ------------------------------------------------------------------- formats
bool format_block(D3DFORMAT format, uint32_t *bw, uint32_t *bh, uint32_t *bytes) {
    *bw = *bh = 1;
    switch ((uint32_t)format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8: case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_X8L8V8U8:
    case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_A2W10V10U10: case D3DFMT_D32:
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4: case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D24FS8: case D3DFMT_INDEX32: case D3DFMT_G16R16F: case D3DFMT_R32F:
        *bytes = 4; return true;
    case D3DFMT_R8G8B8:
        *bytes = 3; return true;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2: case D3DFMT_X4R4G4B4: case D3DFMT_A8P8: case D3DFMT_A8L8:
    case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1:
    case D3DFMT_D16: case D3DFMT_L16: case D3DFMT_INDEX16: case D3DFMT_R16F:
        *bytes = 2; return true;
    case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4:
        *bytes = 1; return true;
    case D3DFMT_A16B16G16R16: case D3DFMT_Q16W16V16U16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F:
        *bytes = 8; return true;
    case D3DFMT_A32B32G32R32F:
        *bytes = 16; return true;
    case D3DFMT_DXT1: case MAKEFOURCC('A', 'T', 'I', '1'):
        *bw = *bh = 4; *bytes = 8; return true;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: case MAKEFOURCC('A', 'T', 'I', '2'):
        *bw = *bh = 4; *bytes = 16; return true;
    case D3DFMT_UYVY: case D3DFMT_YUY2: case D3DFMT_R8G8_B8G8: case D3DFMT_G8R8_G8B8:
        *bw = 2; *bytes = 4; return true;
    default:
        return false;
    }
}

// ----------------------------------------------------------- struct conversion
// D3DPRESENT_PARAMETERS, 32-bit layout (56 bytes): hDeviceWindow is a 4-byte HWND.
void present_from_guest(uint32_t va, D3DPRESENT_PARAMETERS *pp) {
    const uint32_t *g = (const uint32_t *)(g_base + va);
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = g[0]; pp->BackBufferHeight = g[1];
    pp->BackBufferFormat = (D3DFORMAT)g[2]; pp->BackBufferCount = g[3];
    pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)g[4]; pp->MultiSampleQuality = g[5];
    pp->SwapEffect = (D3DSWAPEFFECT)g[6]; pp->hDeviceWindow = hwnd_of(g[7]);
    pp->Windowed = (WINBOOL)g[8]; pp->EnableAutoDepthStencil = (WINBOOL)g[9];
    pp->AutoDepthStencilFormat = (D3DFORMAT)g[10]; pp->Flags = g[11];
    pp->FullScreen_RefreshRateInHz = g[12]; pp->PresentationInterval = g[13];
}
// Writes everything back except the window handle (the guest keeps its HWND).
void present_to_guest(uint32_t va, const D3DPRESENT_PARAMETERS *pp, bool write_window) {
    uint32_t *g = (uint32_t *)(g_base + va);
    g[0] = pp->BackBufferWidth; g[1] = pp->BackBufferHeight;
    g[2] = (uint32_t)pp->BackBufferFormat; g[3] = pp->BackBufferCount;
    g[4] = (uint32_t)pp->MultiSampleType; g[5] = pp->MultiSampleQuality;
    g[6] = (uint32_t)pp->SwapEffect;
    if (write_window) g[7] = pp->hDeviceWindow ? g_guest_hwnd : 0;
    g[8] = (uint32_t)pp->Windowed; g[9] = (uint32_t)pp->EnableAutoDepthStencil;
    g[10] = (uint32_t)pp->AutoDepthStencilFormat; g[11] = pp->Flags;
    g[12] = pp->FullScreen_RefreshRateInHz; g[13] = pp->PresentationInterval;
}
void log_present_parameters(const char *what, const D3DPRESENT_PARAMETERS *pp, HRESULT hr) {
    LOGF("D3D9T %s hr=0x%08X backbuffer=%ux%u fmt=%u count=%u ms=%u/%u swap=%u windowed=%d autoDS=%d/%u flags=0x%X hz=%u interval=0x%X",
         what, (unsigned)hr, (unsigned)pp->BackBufferWidth, (unsigned)pp->BackBufferHeight, (unsigned)pp->BackBufferFormat,
         (unsigned)pp->BackBufferCount, (unsigned)pp->MultiSampleType, (unsigned)pp->MultiSampleQuality, (unsigned)pp->SwapEffect,
         (int)pp->Windowed, (int)pp->EnableAutoDepthStencil, (unsigned)pp->AutoDepthStencilFormat, (unsigned)pp->Flags,
         (unsigned)pp->FullScreen_RefreshRateInHz, (unsigned)pp->PresentationInterval);
}

// ------------------------------------------------------------ special thunks
int S_QueryInterface(const uint32_t *a, uint32_t *r);
int S_Release(const uint32_t *a, uint32_t *r);
int S_GetAdapterIdentifier(const uint32_t *a, uint32_t *r);
int S_GetAdapterMonitor(const uint32_t *a, uint32_t *r);
int S_CreateDevice(const uint32_t *a, uint32_t *r);
int S_GetCreationParameters(const uint32_t *a, uint32_t *r);
int S_CreateAdditionalSwapChain(const uint32_t *a, uint32_t *r);
int S_Reset(const uint32_t *a, uint32_t *r);
HWND present_window();                           // waits for the Android surface (machine released by the caller)
/* The machine (runtime/static_threads.c): one guest thread runs at a time. A
 * call that blocks in the driver gives it up for the duration, exactly like a
 * blocking Win32 call. The game creates its device with
 * D3DCREATE_MULTITHREADED, so DXVK serialises concurrent calls itself. */
extern "C" int st_machine_release(void);
extern "C" void st_machine_reacquire(int held);
extern "C" void st_isolate_host_threads(void);   // runtime/static_threads.c
extern "C" const char *port_act_enter(const char *what);   // runtime/port_prof.c
extern "C" void port_act_leave(const char *previous);
extern "C" int port_stats_active(void);
extern "C" int port_sim_hz;                      // runtime/static_entry.c (port patch sim-rate)
extern "C" void port_bridge_stat(const char *name, uint64_t ns);
int S_DevicePresent(const uint32_t *a, uint32_t *r);
int S_SwapChainPresent(const uint32_t *a, uint32_t *r);
int S_GetPresentParameters(const uint32_t *a, uint32_t *r);
int S_SurfaceLockRect(const uint32_t *a, uint32_t *r);
int S_SurfaceUnlockRect(const uint32_t *a, uint32_t *r);
int S_GetContainer(const uint32_t *a, uint32_t *r);
int S_VolumeLockBox(const uint32_t *a, uint32_t *r);
int S_VolumeUnlockBox(const uint32_t *a, uint32_t *r);
int S_TextureLockRect(const uint32_t *a, uint32_t *r);
int S_TextureUnlockRect(const uint32_t *a, uint32_t *r);
int S_CubeLockRect(const uint32_t *a, uint32_t *r);
int S_CubeUnlockRect(const uint32_t *a, uint32_t *r);
int S_VolumeTextureLockBox(const uint32_t *a, uint32_t *r);
int S_VolumeTextureUnlockBox(const uint32_t *a, uint32_t *r);
int S_VertexBufferLock(const uint32_t *a, uint32_t *r);
int S_VertexBufferUnlock(const uint32_t *a, uint32_t *r);
int S_IndexBufferLock(const uint32_t *a, uint32_t *r);
int S_IndexBufferUnlock(const uint32_t *a, uint32_t *r);

#include "d3d9_thunk_gen.inc"

static_assert(offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) == 1096, "D3DADAPTER_IDENTIFIER9 layout");

inline uint32_t trap_of(uint32_t iface, uint32_t slot) { return D3D9T_TRAP_BASE + (iface << 12) + slot * 8u; }
inline uint32_t vtable_of(uint32_t iface) { return kVtableBase + iface * kVtableStride; }

int interface_by_iid(const GUID &iid) {
    for (int i = 0; i < IF_COUNT; i++)
        if (!memcmp(&g_interfaces[i].iid, &iid, sizeof(GUID))) return i;
    return -1;
}

// Host object -> guest object. One guest object per host object, so pointer
// identity holds for the game exactly as it does on Windows. The guest object
// carries no reference of its own: AddRef/Release go straight to DXVK.
uint32_t wrap_typed(IUnknown *host, int iface) {
    if (!host) return 0;
    if (iface == IF_IDirect3DBaseTexture9 || iface == IF_IDirect3DResource9) {
        switch (((IDirect3DResource9 *)host)->GetType()) {
        case D3DRTYPE_TEXTURE: iface = IF_IDirect3DTexture9; break;
        case D3DRTYPE_CUBETEXTURE: iface = IF_IDirect3DCubeTexture9; break;
        case D3DRTYPE_VOLUMETEXTURE: iface = IF_IDirect3DVolumeTexture9; break;
        case D3DRTYPE_SURFACE: iface = IF_IDirect3DSurface9; break;
        case D3DRTYPE_VOLUME: iface = IF_IDirect3DVolume9; break;
        case D3DRTYPE_VERTEXBUFFER: iface = IF_IDirect3DVertexBuffer9; break;
        case D3DRTYPE_INDEXBUFFER: iface = IF_IDirect3DIndexBuffer9; break;
        default: LOGF("D3D9T wrap: unknown resource type"); return 0;
        }
    }
    std::lock_guard<std::mutex> guard(g_lock);
    auto found = g_by_host.find(host);
    uint32_t index;
    if (found != g_by_host.end()) {
        index = found->second;
        Object *object = g_objects[index];
        if (object->iface != iface) {               // address reused by another kind of object
            object->iface = (uint16_t)iface;
            W32(kObjectBase + index * kObjectSize, vtable_of((uint32_t)iface));
            W32(kObjectBase + index * kObjectSize + 12, (uint32_t)iface);
        }
        return kObjectBase + index * kObjectSize;
    }
    index = g_object_count.load(std::memory_order_relaxed);
    if (index >= kMaxObjects) { LOGF("D3D9T object table full"); return 0; }
    Object *object = new Object();
    object->host = host; object->iface = (uint16_t)iface;
    g_objects[index] = object;
    g_object_count.store(index + 1, std::memory_order_release);
    g_by_host[host] = index;
    uint32_t va = kObjectBase + index * kObjectSize;
    W32(va, vtable_of((uint32_t)iface)); W32(va + 4, kObjectMagic); W32(va + 8, index); W32(va + 12, (uint32_t)iface);
    return va;
}

// ----------------------------------------------------------------- IUnknown
int S_QueryInterface(const uint32_t *a, uint32_t *r) {
    Object *object = object_of(a[0]);
    if (!object) return stop("unknown-this");
    if (!a[1] || !a[2]) { *r = (uint32_t)E_POINTER; return 0; }
    GUID iid; memcpy(&iid, g_base + a[1], sizeof iid);
    void *out = nullptr;
    HRESULT hr = object->host->QueryInterface(iid, &out);
    if (FAILED(hr) || !out) { W32(a[2], 0); *r = (uint32_t)hr; return 0; }
    if (out == (void *)object->host) { W32(a[2], a[0]); *r = (uint32_t)hr; return 0; }
    int iface = interface_by_iid(iid);
    if (iface < 0) { ((IUnknown *)out)->Release(); return stop("queryinterface-foreign-interface"); }
    W32(a[2], wrap_typed((IUnknown *)out, iface));
    *r = (uint32_t)hr;
    return 0;
}

int S_Release(const uint32_t *a, uint32_t *r) {
    Object *object = object_of(a[0]);
    if (!object) return stop("unknown-this");
    ULONG left = object->host->Release();
    *r = (uint32_t)left;
    if (left == 0) {
        // The game holds no reference any more. DXVK may keep the object alive
        // (bound to the device) and may hand the same pointer out again, so the
        // guest object stays; only the guest-side lock memory goes.
        uint32_t shadow = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (object->locks.empty()) { shadow = object->shadow; object->shadow = 0; object->shadow_size = 0; }
        }
        ga_free(shadow);
    }
    return 0;
}

// ---------------------------------------------------------------- IDirect3D9
int S_GetAdapterIdentifier(const uint32_t *a, uint32_t *r) {
    IDirect3D9 *self = (IDirect3D9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[3]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    D3DADAPTER_IDENTIFIER9 id; memset(&id, 0, sizeof id);
    HRESULT hr = self->GetAdapterIdentifier(a[1], a[2], &id);
    if (SUCCEEDED(hr)) memcpy(g_base + a[3], &id, 1100);   // 32-bit size (pack 4)
    *r = (uint32_t)hr;
    return 0;
}

int S_GetAdapterMonitor(const uint32_t *a, uint32_t *r) {
    IDirect3D9 *self = (IDirect3D9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    HMONITOR monitor = self->GetAdapterMonitor(a[1]);
    *r = monitor ? 0x00010002u : 0;                 // opaque guest HMONITOR (one display)
    return 0;
}

int S_CreateDevice(const uint32_t *a, uint32_t *r) {
    IDirect3D9 *self = (IDirect3D9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[5] || !a[6]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    if (!g_native_window) {
        // The surface can come after the game is already creating its device
        // (activity started behind the notification shade, or recreated):
        // wait for it like Present does instead of stopping.
        int machine = st_machine_release();
        present_window();
        st_machine_reacquire(machine);
    }
    D3DPRESENT_PARAMETERS pp;
    if (!R32(a[5] + 28) && a[3]) g_guest_hwnd = a[3];
    present_from_guest(a[5], &pp);
    IDirect3DDevice9 *device = nullptr;
    HRESULT hr = self->CreateDevice(a[1], (D3DDEVTYPE)a[2], hwnd_of(a[3]), a[4], &pp, &device);
    LOGF("D3D9T CreateDevice adapter=%u type=%u focus=0x%08X behavior=0x%08X", (unsigned)a[1], (unsigned)a[2], (unsigned)a[3], (unsigned)a[4]);
    log_present_parameters("CreateDevice", &pp, hr);
    present_to_guest(a[5], &pp, false);
    W32(a[6], SUCCEEDED(hr) ? wrap_typed(device, IF_IDirect3DDevice9) : 0);
    *r = (uint32_t)hr;
    return 0;
}

// ---------------------------------------------------------- IDirect3DDevice9
int S_GetCreationParameters(const uint32_t *a, uint32_t *r) {
    IDirect3DDevice9 *self = (IDirect3DDevice9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[1]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    D3DDEVICE_CREATION_PARAMETERS cp; memset(&cp, 0, sizeof cp);
    HRESULT hr = self->GetCreationParameters(&cp);
    if (SUCCEEDED(hr)) {
        W32(a[1], cp.AdapterOrdinal); W32(a[1] + 4, (uint32_t)cp.DeviceType);
        W32(a[1] + 8, cp.hFocusWindow ? g_guest_hwnd : 0); W32(a[1] + 12, cp.BehaviorFlags);
    }
    *r = (uint32_t)hr;
    return 0;
}

int S_CreateAdditionalSwapChain(const uint32_t *a, uint32_t *r) {
    IDirect3DDevice9 *self = (IDirect3DDevice9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[1] || !a[2]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    D3DPRESENT_PARAMETERS pp; present_from_guest(a[1], &pp);
    IDirect3DSwapChain9 *chain = nullptr;
    HRESULT hr = self->CreateAdditionalSwapChain(&pp, &chain);
    log_present_parameters("CreateAdditionalSwapChain", &pp, hr);
    present_to_guest(a[1], &pp, false);
    W32(a[2], SUCCEEDED(hr) ? wrap_typed(chain, IF_IDirect3DSwapChain9) : 0);
    *r = (uint32_t)hr;
    return 0;
}

int S_Reset(const uint32_t *a, uint32_t *r) {
    IDirect3DDevice9 *self = (IDirect3DDevice9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[1]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    if (!g_native_window) { int machine = st_machine_release(); present_window(); st_machine_reacquire(machine); }
    D3DPRESENT_PARAMETERS pp; present_from_guest(a[1], &pp);
    HRESULT hr = self->Reset(&pp);
    log_present_parameters("Reset", &pp, hr);
    present_to_guest(a[1], &pp, false);
    *r = (uint32_t)hr;
    return 0;
}

// Frame statistics and the optional backbuffer dump (PORT_D3D9_DUMP=<dir>,
// PORT_D3D9_DUMP_EVERY=<n>): raw BGRA with a 16-byte header "D3D9" w h frame.
struct FrameStats {
    uint64_t frames = 0, window_frames = 0;
    double window_start = 0;
    const char *dump_dir = nullptr; uint32_t dump_every = 60;
    IDirect3DSurface9 *copy = nullptr; uint32_t copy_w = 0, copy_h = 0; D3DFORMAT copy_format = D3DFMT_UNKNOWN;
    double present_sum = 0, present_max = 0, pace_sum = 0;   // seconds in the driver's Present / in pace_present, this window
} g_frames;

double now_seconds() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

void dump_backbuffer(IDirect3DDevice9 *device) {
    IDirect3DSurface9 *back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    D3DSURFACE_DESC desc; memset(&desc, 0, sizeof desc);
    back->GetDesc(&desc);
    FrameStats &f = g_frames;
    if (f.copy && (f.copy_w != desc.Width || f.copy_h != desc.Height || f.copy_format != desc.Format)) { f.copy->Release(); f.copy = nullptr; }
    if (!f.copy) {
        if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &f.copy, nullptr))) f.copy = nullptr;
        f.copy_w = desc.Width; f.copy_h = desc.Height; f.copy_format = desc.Format;
    }
    HRESULT hr = f.copy ? device->GetRenderTargetData(back, f.copy) : E_FAIL;
    D3DLOCKED_RECT lr; memset(&lr, 0, sizeof lr);
    if (SUCCEEDED(hr) && SUCCEEDED(f.copy->LockRect(&lr, nullptr, D3DLOCK_READONLY)) && lr.pBits) {
        char path[512];
        snprintf(path, sizeof path, "%s/frame-%06llu.bgra", f.dump_dir, (unsigned long long)f.frames);
        FILE *file = fopen(path, "wb");
        if (file) {
            uint32_t header[4] = { kObjectMagic, desc.Width, desc.Height, (uint32_t)desc.Format };
            fwrite(header, 1, sizeof header, file);
            for (uint32_t y = 0; y < desc.Height; y++) fwrite((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, 1, (size_t)desc.Width * 4, file);
            fclose(file);
            LOGF("D3D9T dumped frame %llu (%ux%u fmt %u) to %s", (unsigned long long)f.frames, (unsigned)desc.Width, (unsigned)desc.Height, (unsigned)desc.Format, path);
        }
        f.copy->UnlockRect();
    } else LOGF("D3D9T frame dump failed hr=0x%08X (multisampled backbuffer?)", (unsigned)hr);
    back->Release();
}


// ---------------------------------------------------------------------------
// Draw-call inspector (development):
//   PORT_D3D9_DRAWLOG=<frame>          one line per draw call of that frame:
//                                      render target, shaders, textures, states
//   PORT_D3D9_DRAWLOG_MATCH=<name>     for draws whose vertex or pixel shader has
//                                      a constant of that name (CTAB), also print
//                                      every named constant with its values and
//                                      write the bound textures (level 0) to
//                                      PORT_D3D9_DUMP/tex-<frame>-<draw>-s<n>.bin
// Everything is read back from DXVK (Get*), nothing is tracked on the side.
struct DrawLog {
    long long frame = -1; const char *match = nullptr; unsigned draw = 0;
} g_drawlog;

uint64_t fnv64(const void *data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++) { h ^= ((const uint8_t *)data)[i]; h *= 1099511628211ull; }
    return h;
}
struct CtabEntry { std::string name; unsigned set, index, count; };
// The CTAB comment D3DX puts in front of the code: named constants and their registers.
std::vector<CtabEntry> ctab_of(const std::vector<uint32_t> &code) {
    std::vector<CtabEntry> out;
    for (size_t pos = 1; pos < code.size();) {
        uint32_t token = code[pos];
        if ((token & 0xFFFFu) != 0xFFFEu) break;
        uint32_t words = (token >> 16) & 0x7FFFu;
        if (pos + 1 + words > code.size()) break;
        const uint8_t *body = (const uint8_t *)&code[pos + 1];
        if (words >= 8 && !memcmp(body, "CTAB", 4)) {
            const uint8_t *tab = body + 4; size_t size = (words - 1) * 4;
            uint32_t count, info;
            memcpy(&count, tab + 12, 4); memcpy(&info, tab + 16, 4);
            for (uint32_t i = 0; i < count && info + (i + 1) * 20 <= size; i++) {
                const uint8_t *e = tab + info + i * 20;
                uint32_t name; uint16_t set, index, regs;
                memcpy(&name, e, 4); memcpy(&set, e + 4, 2); memcpy(&index, e + 6, 2); memcpy(&regs, e + 8, 2);
                if (name < size) out.push_back({ std::string((const char *)tab + name, strnlen((const char *)tab + name, size - name)), set, index, regs });
            }
        }
        pos += 1 + words;
    }
    return out;
}
template <class Shader> std::vector<uint32_t> code_of(Shader *shader) {
    std::vector<uint32_t> code; UINT size = 0;
    if (shader && SUCCEEDED(shader->GetFunction(nullptr, &size)) && size) { code.resize((size + 3) / 4); shader->GetFunction(code.data(), &size); }
    return code;
}
void drawlog(IDirect3DDevice9 *device, const char *method, const uint32_t *a) {
    DrawLog &d = g_drawlog;
    char line[1024]; int n = 0;
    IDirect3DSurface9 *rt = nullptr; D3DSURFACE_DESC rd; memset(&rd, 0, sizeof rd);
    if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt) { rt->GetDesc(&rd); rt->Release(); }
    IDirect3DVertexShader9 *vs = nullptr; IDirect3DPixelShader9 *ps = nullptr;
    device->GetVertexShader(&vs); device->GetPixelShader(&ps);
    std::vector<uint32_t> vcode = code_of(vs), pcode = code_of(ps);
    n += snprintf(line + n, sizeof line - n, "DRAW f%lld #%u %s prims=%u rt=%ux%u/%u vs=%08x ps=%08x tex=", d.frame, d.draw, method,
                  (unsigned)(!strcmp(method, "DrawIndexedPrimitive") ? a[6] : a[3]), (unsigned)rd.Width, (unsigned)rd.Height, (unsigned)rd.Format,
                  (unsigned)(fnv64(vcode.data(), vcode.size() * 4) >> 32), (unsigned)(fnv64(pcode.data(), pcode.size() * 4) >> 32));
    IDirect3DBaseTexture9 *bound[8] = {};
    for (unsigned s = 0; s < 8; s++) {
        if (FAILED(device->GetTexture(s, &bound[s])) || !bound[s]) continue;
        D3DSURFACE_DESC td; memset(&td, 0, sizeof td);
        D3DRESOURCETYPE type = bound[s]->GetType();
        if (type == D3DRTYPE_TEXTURE) ((IDirect3DTexture9 *)bound[s])->GetLevelDesc(0, &td);
        else if (type == D3DRTYPE_CUBETEXTURE) ((IDirect3DCubeTexture9 *)bound[s])->GetLevelDesc(0, &td);
        n += snprintf(line + n, sizeof line - n, "%u:%s%ux%u/%u ", s, type == D3DRTYPE_CUBETEXTURE ? "cube" : (type == D3DRTYPE_VOLUMETEXTURE ? "vol" : ""),
                      (unsigned)td.Width, (unsigned)td.Height, (unsigned)td.Format);
    }
    DWORD z = 0, zw = 0, ab = 0, src = 0, dst = 0, cw = 0, at = 0;
    device->GetRenderState(D3DRS_ZENABLE, &z); device->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab); device->GetRenderState(D3DRS_SRCBLEND, &src);
    device->GetRenderState(D3DRS_DESTBLEND, &dst); device->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
    device->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
    snprintf(line + n, sizeof line - n, "z=%u zw=%u ab=%u %u/%u cw=%x at=%u", (unsigned)z, (unsigned)zw, (unsigned)ab, (unsigned)src, (unsigned)dst, (unsigned)cw, (unsigned)at);
    fprintf(stderr, "%s\n", line);
    if (d.match) {
        bool hit = false;
        std::vector<CtabEntry> tabs[2] = { ctab_of(vcode), ctab_of(pcode) };
        for (auto &tab : tabs) for (auto &e : tab) if (e.name == d.match) hit = true;
        char hashes[2][16];
        snprintf(hashes[0], sizeof hashes[0], "vs:%08x", (unsigned)(fnv64(vcode.data(), vcode.size() * 4) >> 32));
        snprintf(hashes[1], sizeof hashes[1], "ps:%08x", (unsigned)(fnv64(pcode.data(), pcode.size() * 4) >> 32));
        if (strstr(d.match, hashes[0]) || strstr(d.match, hashes[1])) hit = true;
        for (int which = 0; hit && which < 2; which++)
            for (auto &e : tabs[which]) {
                if (e.set == 2) {
                    float v[16] = {};
                    unsigned regs = e.count > 4 ? 4 : e.count;
                    if (which == 0) device->GetVertexShaderConstantF(e.index, v, regs); else device->GetPixelShaderConstantF(e.index, v, regs);
                    for (unsigned r = 0; r < regs; r++)
                        fprintf(stderr, "DRAWC #%u %s c%u %-28s = %g %g %g %g\n", d.draw, which ? "ps" : "vs", e.index + r, e.name.c_str(), v[r * 4], v[r * 4 + 1], v[r * 4 + 2], v[r * 4 + 3]);
                } else if (e.set == 3) {
                    fprintf(stderr, "DRAWC #%u %s s%u %s\n", d.draw, which ? "ps" : "vs", e.index, e.name.c_str());
                }
            }
        if (hit) {   // vertex layout and the first vertices of stream 0
            IDirect3DVertexDeclaration9 *decl = nullptr;
            if (SUCCEEDED(device->GetVertexDeclaration(&decl)) && decl) {
                D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1]; UINT count = 0;
                if (SUCCEEDED(decl->GetDeclaration(nullptr, &count)) && count <= MAXD3DDECLLENGTH + 1 && SUCCEEDED(decl->GetDeclaration(el, &count)))
                    for (UINT i = 0; i + 1 < count; i++)
                        fprintf(stderr, "DRAWV #%u decl stream=%u off=%u type=%u usage=%u idx=%u\n", d.draw, el[i].Stream, el[i].Offset, el[i].Type, el[i].Usage, el[i].UsageIndex);
                decl->Release();
            }
            IDirect3DVertexBuffer9 *vb = nullptr; UINT offset = 0, stride = 0;
            if (SUCCEEDED(device->GetStreamSource(0, &vb, &offset, &stride)) && vb) {
                D3DVERTEXBUFFER_DESC vd; memset(&vd, 0, sizeof vd); vb->GetDesc(&vd);
                void *bits = nullptr;
                HRESULT hr = vb->Lock(0, 0, &bits, D3DLOCK_READONLY);
                fprintf(stderr, "DRAWV #%u vb size=%u usage=%x pool=%u stride=%u offset=%u lock=0x%08X\n", d.draw, vd.Size, vd.Usage, vd.Pool, stride, offset, (unsigned)hr);
                if (SUCCEEDED(hr) && bits && stride) {
                    for (unsigned v = 0; v < 12 && (offset + (v + 1) * stride) <= vd.Size; v++) {
                        const float *f = (const float *)((const uint8_t *)bits + offset + v * stride);
                        char text[256]; int n = 0;
                        for (unsigned k = 0; k < stride / 4 && k < 8; k++) n += snprintf(text + n, sizeof text - n, " %g", f[k]);
                        fprintf(stderr, "DRAWV #%u v%u%s\n", d.draw, v, text);
                    }
                    vb->Unlock();
                }
                vb->Release();
            }
        }
        for (unsigned s = 0; hit && s < 8 && g_frames.dump_dir; s++) {
            if (!bound[s]) continue;
            char path[512];
            if (bound[s]->GetType() == D3DRTYPE_VOLUMETEXTURE) {
                IDirect3DVolumeTexture9 *v = (IDirect3DVolumeTexture9 *)bound[s];
                D3DVOLUME_DESC vd; memset(&vd, 0, sizeof vd); v->GetLevelDesc(0, &vd);
                D3DLOCKED_BOX lb; memset(&lb, 0, sizeof lb);
                HRESULT hr = v->LockBox(0, &lb, nullptr, D3DLOCK_READONLY);
                fprintf(stderr, "DRAWT #%u s%u volume %ux%ux%u fmt=%u pool=%u levels=%u lock=0x%08X row=%d slice=%d\n", d.draw, s, (unsigned)vd.Width,
                        (unsigned)vd.Height, (unsigned)vd.Depth, (unsigned)vd.Format, (unsigned)vd.Pool, (unsigned)v->GetLevelCount(), (unsigned)hr, lb.RowPitch, lb.SlicePitch);
                if (SUCCEEDED(hr) && lb.pBits) {
                    snprintf(path, sizeof path, "%s/vol-%lld-%u-s%u.bin", g_frames.dump_dir, d.frame, d.draw, s);
                    FILE *f = fopen(path, "wb");
                    if (f) {
                        uint32_t header[6] = { vd.Width, vd.Height, vd.Depth, (uint32_t)vd.Format, (uint32_t)lb.RowPitch, (uint32_t)lb.SlicePitch };
                        fwrite(header, 1, sizeof header, f);
                        fwrite(lb.pBits, 1, (size_t)lb.SlicePitch * vd.Depth, f);
                        fclose(f);
                    }
                    v->UnlockBox(0);
                }
                continue;
            }
            if (bound[s]->GetType() != D3DRTYPE_TEXTURE) continue;
            IDirect3DTexture9 *t = (IDirect3DTexture9 *)bound[s];
            {   // render targets: read back through a system-memory copy
                D3DSURFACE_DESC td; memset(&td, 0, sizeof td); t->GetLevelDesc(0, &td);
                if (td.Pool == D3DPOOL_DEFAULT && (td.Usage & D3DUSAGE_RENDERTARGET)) {
                    IDirect3DSurface9 *level = nullptr, *copy = nullptr;
                    HRESULT hr = t->GetSurfaceLevel(0, &level);
                    if (SUCCEEDED(hr)) hr = device->CreateOffscreenPlainSurface(td.Width, td.Height, td.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
                    if (SUCCEEDED(hr)) hr = device->GetRenderTargetData(level, copy);
                    D3DLOCKED_RECT lr; memset(&lr, 0, sizeof lr);
                    if (SUCCEEDED(hr)) hr = copy->LockRect(&lr, nullptr, D3DLOCK_READONLY);
                    fprintf(stderr, "DRAWT #%u s%u rendertarget %ux%u fmt=%u read=0x%08X pitch=%d\n", d.draw, s, (unsigned)td.Width, (unsigned)td.Height,
                            (unsigned)td.Format, (unsigned)hr, (int)lr.Pitch);
                    if (SUCCEEDED(hr) && lr.pBits) {
                        snprintf(path, sizeof path, "%s/rt-%lld-%u-s%u.bin", g_frames.dump_dir, d.frame, d.draw, s);
                        FILE *f = fopen(path, "wb");
                        if (f) {
                            uint32_t header[4] = { td.Width, td.Height, (uint32_t)td.Format, (uint32_t)lr.Pitch };
                            fwrite(header, 1, sizeof header, f);
                            fwrite(lr.pBits, 1, (size_t)lr.Pitch * td.Height, f);
                            fclose(f);
                        }
                        copy->UnlockRect();
                    }
                    if (copy) copy->Release();
                    if (level) level->Release();
                    continue;
                }
            }
            D3DSURFACE_DESC td; memset(&td, 0, sizeof td); t->GetLevelDesc(0, &td);
            D3DLOCKED_RECT lr; memset(&lr, 0, sizeof lr);
            HRESULT hr = t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY);
            fprintf(stderr, "DRAWT #%u s%u %ux%u fmt=%u usage=%x pool=%u levels=%u lock=0x%08X pitch=%d\n", d.draw, s, (unsigned)td.Width, (unsigned)td.Height,
                    (unsigned)td.Format, (unsigned)td.Usage, (unsigned)td.Pool, (unsigned)t->GetLevelCount(), (unsigned)hr, (int)lr.Pitch);
            if (SUCCEEDED(hr) && lr.pBits) {
                char path[512]; snprintf(path, sizeof path, "%s/tex-%lld-%u-s%u.bin", g_frames.dump_dir, d.frame, d.draw, s);
                FILE *f = fopen(path, "wb");
                if (f) {
                    uint32_t header[4] = { td.Width, td.Height, (uint32_t)td.Format, (uint32_t)lr.Pitch };
                    fwrite(header, 1, sizeof header, f);
                    bool dxt = td.Format == D3DFMT_DXT1 || td.Format == D3DFMT_DXT3 || td.Format == D3DFMT_DXT5;
                    fwrite(lr.pBits, 1, (size_t)lr.Pitch * (dxt ? (td.Height + 3) / 4 : td.Height), f);
                    fclose(f);
                }
                t->UnlockRect(0);
            }
        }
    }
    for (auto *t : bound) if (t) t->Release();
    if (vs) vs->Release();
    if (ps) ps->Release();
    d.draw++;
}

void frame_presented(IDirect3DDevice9 *device, HRESULT hr) {
    FrameStats &f = g_frames;
    // PORT_D3D9_DUMP=<dir>: creating <dir>/drawlog.trigger logs (and dumps) the
    // next frame, so a capture can be taken at the moment something is on screen.
    if (f.dump_dir && f.frames % 30 == 0) {
        char trigger[512];
        snprintf(trigger, sizeof trigger, "%s/drawlog.trigger", f.dump_dir);
        if (!access(trigger, F_OK)) {
            unlink(trigger);
            g_drawlog.frame = (long long)f.frames + 1;
            g_drawlog.draw = 0;
            if (!g_drawlog.match) g_drawlog.match = getenv("PORT_D3D9_DRAWLOG_MATCH");
            LOGF("D3D9T drawlog armed for frame %lld", g_drawlog.frame);
        }
    }
    if (f.dump_dir && g_drawlog.frame >= 0 && (long long)f.frames == g_drawlog.frame && device) dump_backbuffer(device);
    double now = now_seconds();
    if (!f.frames) { f.window_start = now; LOGF("D3D9T first Present hr=0x%08X", (unsigned)hr); }
    f.frames++; f.window_frames++;
    if (now - f.window_start >= 5.0) {
        LOGF("D3D9T fps=%.1f frames=%llu calls=%llu objects=%u lock-mem=%lluMiB(peak %llu) present=%.1f/%.1fms pace=%.1fms",
             f.window_frames / (now - f.window_start),
             (unsigned long long)f.frames, (unsigned long long)g_calls.load(), (unsigned)g_object_count.load(),
             (unsigned long long)(g_arena.in_use >> 20), (unsigned long long)(g_arena.peak >> 20),
             f.window_frames ? 1000.0 * f.present_sum / f.window_frames : 0.0, 1000.0 * f.present_max,
             f.window_frames ? 1000.0 * f.pace_sum / f.window_frames : 0.0);
        f.window_start = now; f.window_frames = 0;
        f.present_sum = f.present_max = f.pace_sum = 0;
        st_isolate_host_threads();               // DXVK/audio threads created meanwhile leave the guest core
    }
    (void)device;
}

// The Android surface comes and goes with the activity (another app in front,
// settings opened, activity recreated). Without one, Present waits for the next
// surface instead of presenting into a released one; afterwards it always
// presents to the current surface (DXVK keeps one presenter per window).
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
HWND present_window() {                          // called with the machine released
    std::unique_lock<std::mutex> lock(g_window_mutex);
    if (!g_native_window) {
        LOGF("D3D9T Present: no window (app in the background), waiting for it");
        g_window_cv.wait(lock, [] { return g_native_window != nullptr; });
        LOGF("D3D9T Present: window %p is back", g_native_window);
    }
    return (HWND)g_native_window;
}

// Present cadence of the display the game was told about. The game gets a
// 60 Hz mode (DXVK's Android WSI reports 60/1) and PRESENTATION_INTERVAL_ONE,
// i.e. Present returns on a 60 Hz vblank; its threads are tuned to that (the
// other guest threads run while the presenting thread waits). On a 120 Hz panel
// Present returned twice as early and the waiting moved into the game's own
// 60 fps limiter, which holds the one guest CPU. Present therefore keeps the
// 60 Hz rhythm itself: after the driver call it sleeps (machine released) up to
// the next 1/60 s slot. On a 60 Hz panel the vblank wait already did that.
// PORT_PRESENT_HZ=<hz> changes the rate, 0 turns it off.
struct PresentPacer { double period = -1, next = 0; } g_pacer;
void pace_present() {                            // called with the machine released
    if (g_pacer.period < 0) {
        const char *hz = getenv("PORT_PRESENT_HZ");
        double rate = hz ? atof(hz) : (double)port_sim_hz;
        g_pacer.period = rate > 0 ? 1.0 / rate : 0;
        LOGF("D3D9T present pacing %s", rate > 0 ? (std::to_string(rate) + " Hz").c_str() : "off");
    }
    if (g_pacer.period == 0) return;
    double now = now_seconds();
    if (now < g_pacer.next) {
        double wait = g_pacer.next - now;
        timespec ts = { (time_t)wait, (long)((wait - (double)(time_t)wait) * 1e9) };
        while (nanosleep(&ts, &ts) != 0) { }
        g_pacer.next += g_pacer.period;
    } else {
        // Late by less than a period: keep the rhythm; otherwise start over.
        g_pacer.next = now - g_pacer.next < g_pacer.period ? g_pacer.next + g_pacer.period : now + g_pacer.period;
    }
}

int S_DevicePresent(const uint32_t *a, uint32_t *r) {
    IDirect3DDevice9 *self = (IDirect3DDevice9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (g_frames.dump_dir && g_frames.frames % g_frames.dump_every == 0) dump_backbuffer(self);
    int machine = st_machine_release();          // Present blocks (vsync, frame pacing): the other guest threads run meanwhile
    HWND window = present_window();
    double t_present = now_seconds();
    HRESULT hr = self->Present((const RECT *)P(a[1]), (const RECT *)P(a[2]), window, (const RGNDATA *)P(a[4]));
    double t_pace = now_seconds();
    pace_present();
    double t_done = now_seconds();
    g_frames.present_sum += t_pace - t_present; g_frames.pace_sum += t_done - t_pace;
    if (t_pace - t_present > g_frames.present_max) g_frames.present_max = t_pace - t_present;
    st_machine_reacquire(machine);
    frame_presented(self, hr);
    *r = (uint32_t)hr;
    return 0;
}

int S_SwapChainPresent(const uint32_t *a, uint32_t *r) {
    IDirect3DSwapChain9 *self = (IDirect3DSwapChain9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    IDirect3DDevice9 *device = nullptr;
    if (g_frames.dump_dir && g_frames.frames % g_frames.dump_every == 0 && SUCCEEDED(self->GetDevice(&device)) && device) {
        dump_backbuffer(device);
        device->Release();
    }
    int machine = st_machine_release();
    HWND window = present_window();
    double t_present = now_seconds();
    HRESULT hr = self->Present((const RECT *)P(a[1]), (const RECT *)P(a[2]), window, (const RGNDATA *)P(a[4]), a[5]);
    double t_pace = now_seconds();
    pace_present();
    double t_done = now_seconds();
    g_frames.present_sum += t_pace - t_present; g_frames.pace_sum += t_done - t_pace;
    if (t_pace - t_present > g_frames.present_max) g_frames.present_max = t_pace - t_present;
    st_machine_reacquire(machine);
    frame_presented(nullptr, hr);
    *r = (uint32_t)hr;
    return 0;
}

int S_GetPresentParameters(const uint32_t *a, uint32_t *r) {
    IDirect3DSwapChain9 *self = (IDirect3DSwapChain9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[1]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    D3DPRESENT_PARAMETERS pp; memset(&pp, 0, sizeof pp);
    HRESULT hr = self->GetPresentParameters(&pp);
    if (SUCCEEDED(hr)) present_to_guest(a[1], &pp, true);
    *r = (uint32_t)hr;
    return 0;
}

int S_GetContainer(const uint32_t *a, uint32_t *r) {
    Object *object = object_of(a[0]);
    if (!object) return stop("unknown-this");
    if (!a[1] || !a[2]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    GUID iid; memcpy(&iid, g_base + a[1], sizeof iid);
    void *out = nullptr;
    HRESULT hr = object->iface == IF_IDirect3DSurface9 ? ((IDirect3DSurface9 *)object->host)->GetContainer(iid, &out)
                                                       : ((IDirect3DVolume9 *)object->host)->GetContainer(iid, &out);
    if (FAILED(hr) || !out) { W32(a[2], 0); *r = (uint32_t)hr; return 0; }
    int iface = interface_by_iid(iid);
    if (iface < 0) {
        if (!memcmp(&iid, &g_iid_IDirect3DBaseTexture9, sizeof iid) || !memcmp(&iid, &g_iid_IDirect3DResource9, sizeof iid)) iface = IF_IDirect3DResource9;
        else { ((IUnknown *)out)->Release(); return stop("getcontainer-foreign-interface"); }
    }
    W32(a[2], wrap_typed((IUnknown *)out, iface));
    *r = (uint32_t)hr;
    return 0;
}

// ------------------------------------------------------------- texture locks
// The game gets a guest buffer with the pitch DXVK reported; the bytes are
// copied in at Lock (unless D3DLOCK_DISCARD) and back at Unlock (unless
// D3DLOCK_READONLY).
inline uint64_t lock_key(uint32_t object, uint32_t sub) { return ((uint64_t)object << 32) | sub; }

void copy_rows(uint8_t *dst, const uint8_t *src, const TextureLock &lock) {
    for (uint32_t z = 0; z < lock.slices; z++) {
        uint8_t *d = dst + (size_t)z * lock.slice_pitch;
        const uint8_t *s = src + (size_t)z * lock.slice_pitch;
        if (lock.row_bytes == lock.pitch) { memcpy(d, s, (size_t)lock.rows * lock.pitch); continue; }
        for (uint32_t y = 0; y < lock.rows; y++) memcpy(d + (size_t)y * lock.pitch, s + (size_t)y * lock.pitch, lock.row_bytes);
    }
}

// Registers a successful host lock and returns the guest address of the data.
int begin_texture_lock(uint32_t object, uint32_t sub, D3DFORMAT format, uint32_t width, uint32_t height, uint32_t depth,
                       void *host_bits, int pitch, int slice_pitch, uint32_t flags, uint32_t *guest_bits) {
    uint32_t bw, bh, bytes;
    if (!format_block(format, &bw, &bh, &bytes)) { LOGF("D3D9T lock: unknown format %u", (unsigned)format); return stop("lock-format"); }
    if (pitch <= 0 || !host_bits) return stop("lock-pitch");
    TextureLock lock;
    lock.host = (uint8_t *)host_bits; lock.pitch = (uint32_t)pitch;
    lock.rows = (height + bh - 1) / bh; lock.row_bytes = ((width + bw - 1) / bw) * bytes;
    if (lock.row_bytes > lock.pitch) lock.row_bytes = lock.pitch;
    lock.slices = depth;
    lock.slice_pitch = (depth > 1 && slice_pitch > 0) ? (uint32_t)slice_pitch : lock.rows * lock.pitch;
    lock.write = !(flags & D3DLOCK_READONLY);
    uint64_t total = (uint64_t)lock.slices * lock.slice_pitch;
    if (!total || total > 0x40000000ull) return stop("lock-size");
    lock.guest = ga_alloc((uint32_t)total);
    if (!lock.guest) return stop("lock-arena-exhausted");
    if (!(flags & D3DLOCK_DISCARD)) copy_rows(g_base + lock.guest, lock.host, lock);
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto old = g_texture_locks.find(lock_key(object, sub));
        if (old != g_texture_locks.end()) { uint32_t stale = old->second.guest; g_texture_locks.erase(old); ga_free(stale); }
        g_texture_locks[lock_key(object, sub)] = lock;
    }
    *guest_bits = lock.guest;
    return 0;
}
// Copies the guest's bytes into the host mapping; the caller then unlocks.
void end_texture_lock(uint32_t object, uint32_t sub) {
    TextureLock lock;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto found = g_texture_locks.find(lock_key(object, sub));
        if (found == g_texture_locks.end()) return;
        lock = found->second;
        g_texture_locks.erase(found);
    }
    if (lock.write) copy_rows(lock.host, g_base + lock.guest, lock);
    ga_free(lock.guest);
}

int finish_lock_rect(uint32_t object, uint32_t sub, HRESULT hr, const D3DSURFACE_DESC &desc, const D3DLOCKED_RECT &lr,
                     uint32_t out, uint32_t rect, uint32_t flags, uint32_t *r) {
    *r = (uint32_t)hr;
    if (FAILED(hr)) { if (out) { W32(out, 0); W32(out + 4, 0); } return 0; }
    uint32_t width = desc.Width, height = desc.Height;
    if (rect) { const int32_t *q = (const int32_t *)(g_base + rect); width = (uint32_t)(q[2] - q[0]); height = (uint32_t)(q[3] - q[1]); }
    uint32_t guest = 0;
    if (begin_texture_lock(object, sub, desc.Format, width, height, 1, lr.pBits, lr.Pitch, 0, flags, &guest)) return -1;
    W32(out, (uint32_t)lr.Pitch); W32(out + 4, guest);
    return 0;
}

int S_SurfaceLockRect(const uint32_t *a, uint32_t *r) {       // (this, pLockedRect, pRect, Flags)
    IDirect3DSurface9 *self = (IDirect3DSurface9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[1]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    D3DSURFACE_DESC desc; memset(&desc, 0, sizeof desc); self->GetDesc(&desc);
    D3DLOCKED_RECT lr; memset(&lr, 0, sizeof lr);
    HRESULT hr = self->LockRect(&lr, (const RECT *)P(a[2]), a[3]);
    int rc = finish_lock_rect(a[0], 0, hr, desc, lr, a[1], a[2], a[3], r);
    if (rc) self->UnlockRect();
    return rc;
}
int S_SurfaceUnlockRect(const uint32_t *a, uint32_t *r) {
    IDirect3DSurface9 *self = (IDirect3DSurface9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    end_texture_lock(a[0], 0);
    *r = (uint32_t)self->UnlockRect();
    return 0;
}
int S_TextureLockRect(const uint32_t *a, uint32_t *r) {       // (this, Level, pLockedRect, pRect, Flags)
    IDirect3DTexture9 *self = (IDirect3DTexture9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[2]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    D3DSURFACE_DESC desc; memset(&desc, 0, sizeof desc); self->GetLevelDesc(a[1], &desc);
    D3DLOCKED_RECT lr; memset(&lr, 0, sizeof lr);
    HRESULT hr = self->LockRect(a[1], &lr, (const RECT *)P(a[3]), a[4]);
    int rc = finish_lock_rect(a[0], a[1], hr, desc, lr, a[2], a[3], a[4], r);
    if (rc) self->UnlockRect(a[1]);
    return rc;
}
int S_TextureUnlockRect(const uint32_t *a, uint32_t *r) {     // (this, Level)
    IDirect3DTexture9 *self = (IDirect3DTexture9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    end_texture_lock(a[0], a[1]);
    *r = (uint32_t)self->UnlockRect(a[1]);
    return 0;
}
int S_CubeLockRect(const uint32_t *a, uint32_t *r) {          // (this, Face, Level, pLockedRect, pRect, Flags)
    IDirect3DCubeTexture9 *self = (IDirect3DCubeTexture9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[3]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    D3DSURFACE_DESC desc; memset(&desc, 0, sizeof desc); self->GetLevelDesc(a[2], &desc);
    D3DLOCKED_RECT lr; memset(&lr, 0, sizeof lr);
    HRESULT hr = self->LockRect((D3DCUBEMAP_FACES)a[1], a[2], &lr, (const RECT *)P(a[4]), a[5]);
    int rc = finish_lock_rect(a[0], a[1] * 64u + a[2], hr, desc, lr, a[3], a[4], a[5], r);
    if (rc) self->UnlockRect((D3DCUBEMAP_FACES)a[1], a[2]);
    return rc;
}
int S_CubeUnlockRect(const uint32_t *a, uint32_t *r) {        // (this, Face, Level)
    IDirect3DCubeTexture9 *self = (IDirect3DCubeTexture9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    end_texture_lock(a[0], a[1] * 64u + a[2]);
    *r = (uint32_t)self->UnlockRect((D3DCUBEMAP_FACES)a[1], a[2]);
    return 0;
}

int finish_lock_box(uint32_t object, uint32_t sub, HRESULT hr, const D3DVOLUME_DESC &desc, const D3DLOCKED_BOX &lb,
                    uint32_t out, uint32_t box, uint32_t flags, uint32_t *r) {
    *r = (uint32_t)hr;
    if (FAILED(hr)) { if (out) { W32(out, 0); W32(out + 4, 0); W32(out + 8, 0); } return 0; }
    uint32_t width = desc.Width, height = desc.Height, depth = desc.Depth;
    if (box) { const uint32_t *q = (const uint32_t *)(g_base + box); width = q[2] - q[0]; height = q[3] - q[1]; depth = q[5] - q[4]; }
    uint32_t guest = 0;
    if (begin_texture_lock(object, sub, desc.Format, width, height, depth ? depth : 1, lb.pBits, lb.RowPitch, lb.SlicePitch, flags, &guest)) return -1;
    W32(out, (uint32_t)lb.RowPitch); W32(out + 4, (uint32_t)lb.SlicePitch); W32(out + 8, guest);
    return 0;
}
int S_VolumeLockBox(const uint32_t *a, uint32_t *r) {         // (this, pLockedVolume, pBox, Flags)
    IDirect3DVolume9 *self = (IDirect3DVolume9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[1]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    D3DVOLUME_DESC desc; memset(&desc, 0, sizeof desc); self->GetDesc(&desc);
    D3DLOCKED_BOX lb; memset(&lb, 0, sizeof lb);
    HRESULT hr = self->LockBox(&lb, (const D3DBOX *)P(a[2]), a[3]);
    int rc = finish_lock_box(a[0], 0, hr, desc, lb, a[1], a[2], a[3], r);
    if (rc) self->UnlockBox();
    return rc;
}
int S_VolumeUnlockBox(const uint32_t *a, uint32_t *r) {
    IDirect3DVolume9 *self = (IDirect3DVolume9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    end_texture_lock(a[0], 0);
    *r = (uint32_t)self->UnlockBox();
    return 0;
}
int S_VolumeTextureLockBox(const uint32_t *a, uint32_t *r) {  // (this, Level, pLockedVolume, pBox, Flags)
    IDirect3DVolumeTexture9 *self = (IDirect3DVolumeTexture9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    if (!a[2]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    D3DVOLUME_DESC desc; memset(&desc, 0, sizeof desc); self->GetLevelDesc(a[1], &desc);
    D3DLOCKED_BOX lb; memset(&lb, 0, sizeof lb);
    HRESULT hr = self->LockBox(a[1], &lb, (const D3DBOX *)P(a[3]), a[4]);
    int rc = finish_lock_box(a[0], a[1], hr, desc, lb, a[2], a[3], a[4], r);
    if (rc) self->UnlockBox(a[1]);
    return rc;
}
int S_VolumeTextureUnlockBox(const uint32_t *a, uint32_t *r) {
    IDirect3DVolumeTexture9 *self = (IDirect3DVolumeTexture9 *)host_of(a[0]);
    if (!self) return stop("unknown-this");
    end_texture_lock(a[0], a[1]);
    *r = (uint32_t)self->UnlockBox(a[1]);
    return 0;
}

// -------------------------------------------------------------- buffer locks
// A vertex/index buffer has one guest copy for its whole lifetime (the game is
// the only writer). Lock returns a pointer into it, Unlock copies the locked
// range into the DXVK mapping. Nothing is copied at Lock.
template <class Buffer, class Desc>
int buffer_lock(const uint32_t *a, uint32_t *r) {             // (this, Offset, Size, ppbData, Flags)
    Object *object = object_of(a[0]);
    if (!object) return stop("unknown-this");
    Buffer *self = (Buffer *)object->host;
    if (!a[3]) { *r = (uint32_t)D3DERR_INVALIDCALL; return 0; }
    if (!object->shadow) {
        Desc desc; memset(&desc, 0, sizeof desc);
        if (FAILED(self->GetDesc(&desc)) || !desc.Size) return stop("buffer-desc");
        uint32_t shadow = ga_alloc(desc.Size);
        if (!shadow) return stop("lock-arena-exhausted");
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (!object->shadow) {
                object->shadow = shadow; object->shadow_size = desc.Size; shadow = 0;
                object->dynamic = (desc.Usage & D3DUSAGE_DYNAMIC) != 0;
            }
        }
        ga_free(shadow);                            // raced: another thread installed one
    }
    // A static (non-dynamic) buffer hands the game a pointer into memory that
    // holds the WHOLE buffer, and Windows keeps everything written through it,
    // also past SizeToLock. The game relies on that: its sky dome locks 51
    // bytes (a vertex count, not a size) and writes all 1020 (image 0x00548FA4).
    // So a static buffer is locked from the offset to its end, and the whole
    // range goes to DXVK at Unlock. Dynamic buffers keep the exact range: they
    // are the streaming ones, and NOOVERWRITE/DISCARD depend on it.
    uint32_t size_to_lock = a[2];
    if (!object->dynamic) size_to_lock = 0;
    void *host = nullptr;
    HRESULT hr = self->Lock(a[1], size_to_lock, &host, a[4]);
    *r = (uint32_t)hr;
    if (FAILED(hr) || !host) { W32(a[3], 0); return 0; }
    uint32_t offset = a[1], size = size_to_lock;
    if (offset > object->shadow_size) { self->Unlock(); return stop("buffer-lock-range"); }
    if (!size || size > object->shadow_size - offset) size = object->shadow_size - offset;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        object->locks.push_back(BufferLock{ offset, size, (uint8_t *)host, !(a[4] & D3DLOCK_READONLY) });
    }
    W32(a[3], object->shadow + offset);
    static long trace_size = -1;
    if (trace_size < 0) trace_size = getenv("PORT_TRACE_BUFFER_SIZE") ? atol(getenv("PORT_TRACE_BUFFER_SIZE")) : 0;
    if (trace_size && (long)object->shadow_size == trace_size)
        LOGF("BUFLOCK this=%08X off=%u size=%u flags=0x%X ret=%08X shadow=%08X locks=%u", a[0], a[1], a[2], a[4], a[-1], object->shadow,
             (unsigned)object->locks.size());
    return 0;
}
template <class Buffer>
int buffer_unlock(const uint32_t *a, uint32_t *r) {
    Object *object = object_of(a[0]);
    if (!object) return stop("unknown-this");
    Buffer *self = (Buffer *)object->host;
    BufferLock lock{ 0, 0, nullptr, false };
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!object->locks.empty()) { lock = object->locks.back(); object->locks.pop_back(); }
    }
    if (lock.host && lock.write && object->shadow) memcpy(lock.host, g_base + object->shadow + lock.offset, lock.size);
    static long trace_size = -1;
    if (trace_size < 0) trace_size = getenv("PORT_TRACE_BUFFER_SIZE") ? atol(getenv("PORT_TRACE_BUFFER_SIZE")) : 0;
    if (trace_size && (long)object->shadow_size == trace_size && object->shadow) {
        const float *f = (const float *)(g_base + object->shadow + lock.offset);
        LOGF("BUFUNLOCK this=%08X off=%u size=%u ret=%08X data: %g %g %g %g %g | %g %g %g %g %g | %g %g %g %g %g | %g %g %g %g %g", a[0], lock.offset,
             lock.size, a[-1], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19]);
    }
    *r = (uint32_t)self->Unlock();
    return 0;
}
int S_VertexBufferLock(const uint32_t *a, uint32_t *r) { return buffer_lock<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(a, r); }
int S_VertexBufferUnlock(const uint32_t *a, uint32_t *r) { return buffer_unlock<IDirect3DVertexBuffer9>(a, r); }
int S_IndexBufferLock(const uint32_t *a, uint32_t *r) { return buffer_lock<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(a, r); }
int S_IndexBufferUnlock(const uint32_t *a, uint32_t *r) { return buffer_unlock<IDirect3DIndexBuffer9>(a, r); }

// --------------------------------------------------------------------- DXVK
typedef IDirect3D9 *(WINAPI *Direct3DCreate9Fn)(UINT);
typedef void (*SetDisplayMetricsFn)(uint32_t, uint32_t, uint32_t, uint32_t);
Direct3DCreate9Fn g_create9;
SetDisplayMetricsFn g_set_metrics;
std::once_flag g_load_once;
thread_local char g_stop_text[160];

void load_dxvk() {
    if (!getenv("DXVK_WSI_DRIVER")) setenv("DXVK_WSI_DRIVER", "Android", 1);
    const char *name = getenv("PORT_DXVK_D3D9") ? getenv("PORT_DXVK_D3D9") : "libdxvk_d3d9.so";
    void *library = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!library) { LOGF("D3D9T cannot load %s: %s", name, dlerror()); return; }
    g_create9 = (Direct3DCreate9Fn)dlsym(library, "Direct3DCreate9");
    if (!g_create9) LOGF("D3D9T %s has no Direct3DCreate9: %s", name, dlerror());
    else LOGF("D3D9T loaded %s", name);
    // The display the game enumerates is the surface it will render to; DXVK's
    // Android WSI learns the size from the first window otherwise, which is
    // after the game has already asked for the mode list.
    g_set_metrics = (SetDisplayMetricsFn)dlsym(library, "dxvk_android_wsi_set_display_metrics");
    if (g_set_metrics && g_window_width > 0 && g_window_height > 0)
        g_set_metrics((uint32_t)g_window_width, (uint32_t)g_window_height, (uint32_t)port_sim_hz, 1);
    else if (!g_set_metrics) LOGF("D3D9T %s exports no dxvk_android_wsi_set_display_metrics", name);
}

}  // namespace

// ------------------------------------------------------------------ C entry
extern "C" void d3d9t_init(uint8_t *flat) {
    if (g_base) return;
    g_base = flat;
    g_trace = getenv("PORT_TRACE_D3D") ? atoi(getenv("PORT_TRACE_D3D")) : 0;
    g_frames.dump_dir = getenv("PORT_D3D9_DUMP");
    if (getenv("PORT_D3D9_DRAWLOG")) { g_drawlog.frame = atoll(getenv("PORT_D3D9_DRAWLOG")); g_drawlog.match = getenv("PORT_D3D9_DRAWLOG_MATCH"); }
    if (getenv("PORT_D3D9_DUMP_EVERY") && atoi(getenv("PORT_D3D9_DUMP_EVERY")) > 0) g_frames.dump_every = (uint32_t)atoi(getenv("PORT_D3D9_DUMP_EVERY"));
    for (uint32_t iface = 0; iface < IF_COUNT; iface++)
        for (uint32_t slot = 0; slot < g_interfaces[iface].count; slot++)
            W32(vtable_of(iface) + slot * 4u, trap_of(iface, slot));
    g_arena.free_blocks[kArenaBase] = kArenaLimit - kArenaBase;
    dsoundt_init(flat);
    g_arena.free_blocks[kOverflowBase] = kOverflowLimit - kOverflowBase;
}

// Frames presented so far (runtime/port_prof.c watches the frame rate).
extern "C" uint64_t d3d9t_frames(void) { return g_frames.frames; }

extern "C" void d3d9t_set_native_window(void *native_window, int width, int height) {
    {
        std::lock_guard<std::mutex> lock(g_window_mutex);
        g_native_window = native_window;
        g_window_width = width; g_window_height = height;
    }
    g_window_cv.notify_all();
    if (g_set_metrics && width > 0 && height > 0) g_set_metrics((uint32_t)width, (uint32_t)height, (uint32_t)port_sim_hz, 1);
#ifdef __ANDROID__
    // The frame rate the game produces, for the compositor (API 30; the shell
    // announced 60 before the image knew the simulation rate).
    if (native_window) {
        typedef int32_t (*SetFrameRateFn)(void *, float, int8_t);
        static SetFrameRateFn set_frame_rate = (SetFrameRateFn)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
        if (set_frame_rate) LOGF("D3D9T ANativeWindow_setFrameRate(%d) -> %d", port_sim_hz, set_frame_rate(native_window, (float)port_sim_hz, 1));
    }
#endif
    LOGF("D3D9T native window %p %dx%d", native_window, width, height);
}

extern "C" uint32_t d3d9t_direct3d_create9(uint32_t sdk_version) {
    if (!g_base) return 0;
    std::call_once(g_load_once, load_dxvk);
    if (!g_create9) return 0;
    IDirect3D9 *d3d = g_create9(sdk_version);
    LOGF("D3D9T Direct3DCreate9(%u) -> %p", (unsigned)sdk_version, (void *)d3d);
    return wrap_typed(d3d, IF_IDirect3D9);
}

extern "C" uint32_t d3d9t_guest_alloc(uint32_t size) { return ga_alloc(size); }
extern "C" void d3d9t_guest_free(uint32_t address) { ga_free(address); }
extern "C" uint8_t *d3d9t_base(void) { return g_base; }

extern "C" const char *d3d9t_trap_name(uint32_t trap) {
    static thread_local char text[96];
    if (trap >= DSOUNDT_TRAP_BASE && d3d9t_is_trap(trap)) return dsoundt_trap_name(trap);
    if (trap >= D3DXT_TRAP_BASE && d3d9t_is_trap(trap)) return d3dxt_trap_name(trap);
    uint32_t iface = (trap - D3D9T_TRAP_BASE) >> 12, slot = ((trap - D3D9T_TRAP_BASE) & 0xFFFu) / 8u;
    if (!d3d9t_is_trap(trap) || iface >= IF_COUNT || slot >= g_interfaces[iface].count) return "?";
    snprintf(text, sizeof text, "%s::%s", g_interfaces[iface].name, g_interfaces[iface].methods[slot].name);
    return text;
}

extern "C" const char *d3d9t_last_stop(void) { return t_stop; }
extern "C" uint64_t d3d9t_call_count(void) { return g_calls.load(); }
extern "C" uint32_t d3d9t_live_objects(void) { return g_object_count.load(); }

extern "C" int d3d9t_call(uint32_t trap, const uint32_t *args, uint32_t *eax) {
    if (trap >= DSOUNDT_TRAP_BASE && d3d9t_is_trap(trap)) {
        int rc = dsoundt_call(trap, args, eax);
        if (rc < 0) t_stop = dsoundt_last_stop();
        return rc;
    }
    if (trap >= D3DXT_TRAP_BASE && d3d9t_is_trap(trap)) {
        g_calls.fetch_add(1);
        int rc = d3dxt_call(trap, args, eax);
        if (g_trace) fprintf(stderr, "D3DX %s(%08X, %08X, %08X) -> 0x%08X%s\n", d3dxt_trap_name(trap), args[0], args[1], args[2], *eax, rc < 0 ? " STOP" : "");
        if (rc < 0) t_stop = d3dxt_last_stop();
        return rc;
    }
    uint32_t iface = (trap - D3D9T_TRAP_BASE) >> 12, slot = ((trap - D3D9T_TRAP_BASE) & 0xFFFu) / 8u;
    if (!d3d9t_is_trap(trap) || iface >= IF_COUNT || slot >= g_interfaces[iface].count || ((trap - D3D9T_TRAP_BASE) & 7u)) {
        t_stop = "bad-trap-address";
        return -1;
    }
    const Method &method = g_interfaces[iface].methods[slot];
    uint64_t number = g_calls.fetch_add(1) + 1;
    if (!method.fn) {
        snprintf(g_stop_text, sizeof g_stop_text, "%s::%s: not implemented", g_interfaces[iface].name, method.name);
        t_stop = g_stop_text;
        return -1;
    }
    t_stop = "";
    const char *activity = port_act_enter(method.name);
    int rc;
    if (port_stats_active()) {
        double t0 = now_seconds();
        rc = method.fn(args, eax);
        port_bridge_stat(method.name, (uint64_t)((now_seconds() - t0) * 1e9));
    } else {
        rc = method.fn(args, eax);
    }
    port_act_leave(activity);
    if (g_drawlog.frame >= 0 && (long long)g_frames.frames == g_drawlog.frame && iface == IF_IDirect3DDevice9 && !strncmp(method.name, "Draw", 4)) {
        IDirect3DDevice9 *device = (IDirect3DDevice9 *)host_of(args[0]);
        if (device) drawlog(device, method.name, args);
    }
    if (g_trace)
        fprintf(stderr, "D3D9 #%llu %s::%s(%08X, %08X, %08X, %08X, %08X, %08X) -> 0x%08X%s\n", (unsigned long long)number,
                g_interfaces[iface].name, method.name, args[0], method.argc > 1 ? args[1] : 0, method.argc > 2 ? args[2] : 0,
                method.argc > 3 ? args[3] : 0, method.argc > 4 ? args[4] : 0, method.argc > 5 ? args[5] : 0, *eax, rc ? " STOP" : "");
    if (rc) {
        snprintf(g_stop_text, sizeof g_stop_text, "%s::%s: %s", g_interfaces[iface].name, method.name, t_stop);
        t_stop = g_stop_text;
        return -1;
    }
    return (int)(method.argc * 4u);
}
