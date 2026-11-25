// d3dx9_37.dll: D3DXCompileShader, D3DXGetShaderConstantTable, ID3DXBuffer and
// ID3DXConstantTable for the 32-bit guest.
//
// The HLSL compiler is not re-implemented. A shader is looked up in a private
// cache of results produced by the user's own d3dx9_37.dll (the DirectX
// redistributable that ships with the game); on the development host a miss is
// compiled on the spot by tools/d3dxcc (that DLL, run under Wine) and stored.
// On the device a miss is a named stop.
//
//   key   = SHA-256 over flags, entry point, profile, defines and source
//   files = <cache>/<key>.dxcq (the request) and <key>.dxcr (the response)
//
// The constant table is not invented either: it is the CTAB block that the
// compiler put into the bytecode, read with bounds checks.
#include "d3d9_thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <mutex>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
uint32_t d3d9t_guest_alloc(uint32_t size);
void d3d9t_guest_free(uint32_t address);
uint8_t *d3d9t_base(void);
}

namespace {

constexpr uint32_t kTrapBase = D3DXT_TRAP_BASE;
constexpr uint32_t kVtableBase = 0x6D1F0000u;
constexpr uint32_t kMagic = 0x58443344u;           // "D3DX"
constexpr uint32_t S_OK_ = 0, E_FAIL_ = 0x80004005u, E_NOINTERFACE_ = 0x80004002u,
                   D3DERR_INVALIDCALL_ = 0x8876086Cu, E_OUTOFMEMORY_ = 0x8007000Eu;

enum { IF_BUFFER = 0, IF_TABLE = 1 };
const char *const kBufferMethods[] = { "QueryInterface", "AddRef", "Release", "GetBufferPointer", "GetBufferSize" };
const char *const kTableMethods[] = {
    "QueryInterface", "AddRef", "Release", "GetBufferPointer", "GetBufferSize",
    "GetDesc", "GetConstantDesc", "GetSamplerIndex", "GetConstant", "GetConstantByName", "GetConstantElement",
    "SetDefaults", "SetValue", "SetBool", "SetBoolArray", "SetInt", "SetIntArray", "SetFloat", "SetFloatArray",
    "SetVector", "SetVectorArray", "SetMatrix", "SetMatrixArray", "SetMatrixPointerArray", "SetMatrixTranspose",
    "SetMatrixTransposeArray", "SetMatrixTransposePointerArray" };
// stdcall argument words including `this`
const uint8_t kBufferArgs[] = { 3, 1, 1, 1, 1 };
const uint8_t kTableArgs[] = { 3, 1, 1, 1, 1, 2, 4, 2, 3, 3, 3, 2, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4, 5, 5, 4, 5, 5 };

uint8_t *g_base;
thread_local const char *t_stop = "";
thread_local char t_stop_text[160];
int stop(const char *why) { t_stop = why; return -1; }
inline uint32_t R32(uint32_t va) { uint32_t v; memcpy(&v, g_base + va, 4); return v; }
inline void W32(uint32_t va, uint32_t v) { memcpy(g_base + va, &v, 4); }
#define LOGF(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

// ------------------------------------------------------------------ SHA-256
struct Sha256 {
    uint32_t h[8]; uint8_t block[64]; uint64_t length = 0; size_t fill = 0;
    Sha256() { static const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }; memcpy(h, init, sizeof h); }
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress() {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void *data, size_t size) {
        const uint8_t *p = (const uint8_t *)data;
        length += size;
        while (size) {
            size_t take = 64 - fill < size ? 64 - fill : size;
            memcpy(block + fill, p, take); fill += take; p += take; size -= take;
            if (fill == 64) { compress(); fill = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = length * 8;
        uint8_t pad = 0x80; update(&pad, 1);
        uint8_t zero = 0; while (fill != 56) update(&zero, 1);
        uint8_t tail[8]; for (int i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - i * 8));
        update(tail, 8);
        char text[65];
        for (int i = 0; i < 8; i++) snprintf(text + i * 8, 9, "%08x", h[i]);
        return text;
    }
};

// ------------------------------------------------------------------ objects
struct Constant {
    uint32_t name, type_info, default_value;
    uint16_t register_set, register_index, register_count;
    uint16_t cls, type, rows, columns, elements, members;
};
struct Object {
    int iface; uint32_t refs; uint32_t data, size;          // guest copy of the bytes
    // constant table only
    uint32_t creator = 0, version = 0, handles = 0;
    std::vector<Constant> constants;
};
std::mutex g_lock;
std::unordered_map<uint32_t, Object *> g_objects;           // guest object address -> object

uint32_t make_object(Object *object) {
    uint32_t va = d3d9t_guest_alloc(16);
    if (!va) { delete object; return 0; }
    W32(va, kVtableBase + (uint32_t)object->iface * 0x400u); W32(va + 4, kMagic); W32(va + 8, (uint32_t)object->iface); W32(va + 12, 0);
    std::lock_guard<std::mutex> guard(g_lock);
    g_objects[va] = object;
    return va;
}
Object *object_of(uint32_t va) {
    std::lock_guard<std::mutex> guard(g_lock);
    auto found = g_objects.find(va);
    return found == g_objects.end() ? nullptr : found->second;
}

uint32_t make_buffer(const uint8_t *bytes, uint32_t size) {
    Object *object = new Object();
    object->iface = IF_BUFFER; object->refs = 1; object->size = size;
    object->data = d3d9t_guest_alloc(size ? size : 1);
    if (!object->data) { delete object; return 0; }
    if (size) memcpy(g_base + object->data, bytes, size);
    return make_object(object);
}

// Finds the CTAB comment of a shader: returns the bytes after the "CTAB" tag.
bool find_ctab(const uint8_t *code, size_t limit, const uint8_t **table, uint32_t *size) {
    if (limit < 4) return false;
    size_t pos = 4;                                         // version token
    while (pos + 4 <= limit) {
        uint32_t token; memcpy(&token, code + pos, 4);
        if ((token & 0xFFFFu) == 0xFFFFu) return false;     // end
        if ((token & 0xFFFFu) != 0xFFFEu) return false;     // the table is among the leading comments
        uint32_t words = (token >> 16) & 0x7FFFu;
        if (pos + 4 + (size_t)words * 4 > limit) return false;
        if (words >= 1 && !memcmp(code + pos + 4, "CTAB", 4)) { *table = code + pos + 8; *size = (words - 1) * 4; return true; }
        pos += 4 + (size_t)words * 4;
    }
    return false;
}

// Length of a shader in bytes (up to and including the end token), bounded.
size_t shader_length(const uint8_t *code, size_t limit) {
    size_t pos = 4;
    while (pos + 4 <= limit) {
        uint32_t token; memcpy(&token, code + pos, 4);
        pos += 4;
        if (token == 0x0000FFFFu) return pos;
        if ((token & 0xFFFFu) == 0xFFFEu) pos += (size_t)((token >> 16) & 0x7FFFu) * 4;
    }
    return 0;
}

uint32_t make_table(const uint8_t *table, uint32_t size) {
    if (size < 28) return 0;
    uint32_t header[7]; memcpy(header, table, sizeof header);
    uint32_t count = header[3], info = header[4];
    if (header[0] != 28 || (uint64_t)info + (uint64_t)count * 20 > size || header[1] >= size) return 0;
    Object *object = new Object();
    object->iface = IF_TABLE; object->refs = 1; object->size = size;
    object->creator = header[1]; object->version = header[2];
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = table + info + i * 20;
        Constant c{};
        memcpy(&c.name, entry, 4); memcpy(&c.register_set, entry + 4, 2); memcpy(&c.register_index, entry + 6, 2);
        memcpy(&c.register_count, entry + 8, 2); memcpy(&c.type_info, entry + 12, 4); memcpy(&c.default_value, entry + 16, 4);
        if (c.name >= size || (uint64_t)c.type_info + 16 > size || c.default_value >= size) { delete object; return 0; }
        const uint8_t *type = table + c.type_info;
        memcpy(&c.cls, type, 2); memcpy(&c.type, type + 2, 2); memcpy(&c.rows, type + 4, 2);
        memcpy(&c.columns, type + 6, 2); memcpy(&c.elements, type + 8, 2); memcpy(&c.members, type + 10, 2);
        object->constants.push_back(c);
    }
    object->data = d3d9t_guest_alloc(size);
    object->handles = d3d9t_guest_alloc(count ? count * 4 : 4);
    if (!object->data || !object->handles) { delete object; return 0; }
    memcpy(g_base + object->data, table, size);
    for (uint32_t i = 0; i < count; i++) W32(object->handles + i * 4, kMagic ^ i);
    return make_object(object);
}

// A D3DXHANDLE of this table, or the name of a top-level constant.
int resolve(Object *table, uint32_t handle, int *index) {
    uint32_t count = (uint32_t)table->constants.size();
    if (handle >= table->handles && handle < table->handles + count * 4 && !((handle - table->handles) & 3)) {
        *index = (int)((handle - table->handles) / 4);
        return 0;
    }
    if (handle < 0x10000u) { *index = -1; return 0; }
    const char *name = (const char *)g_base + handle;
    size_t length = strnlen(name, 256);
    if (length == 256) { *index = -1; return 0; }
    if (strchr(name, '.') || strchr(name, '[')) return stop("constant-table-nested-name");
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(name, (const char *)g_base + table->data + table->constants[i].name)) { *index = (int)i; return 0; }
    *index = -1;
    return 0;
}

// --------------------------------------------------------------- compiling
std::string g_cache_dir, g_helper;
std::mutex g_compile_lock;

bool read_file(const std::string &path, std::vector<uint8_t> *out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    out->resize(size > 0 ? (size_t)size : 0);
    bool ok = !size || fread(out->data(), 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    return ok;
}
bool write_file(const std::string &path, const std::vector<uint8_t> &data) {
    std::string temp = path + ".tmp";
    FILE *f = fopen(temp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = !fclose(f) && ok;
    return ok && !rename(temp.c_str(), path.c_str());
}
void put32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (i * 8))); }
void putz(std::vector<uint8_t> &v, const char *s) { v.insert(v.end(), s, s + strlen(s) + 1); }

const char *guest_string(uint32_t va, size_t limit) {
    if (!va) return "";
    const char *s = (const char *)g_base + va;
    return strnlen(s, limit) == limit ? nullptr : s;
}

int compile_shader(const uint32_t *a, uint32_t *r) {
    // (pSrcData, SrcDataLen, pDefines, pInclude, pFunctionName, pProfile, Flags, ppShader, ppErrorMsgs, ppConstantTable)
    uint32_t source = a[0], length = a[1], defines = a[2], flags = a[6];
    if (a[3]) return stop("D3DXCompileShader: ID3DXInclude handler");
    const char *entry = guest_string(a[4], 256), *profile = guest_string(a[5], 64);
    if (!source || !entry || !profile || !*profile || length > (16u << 20)) { *r = D3DERR_INVALIDCALL_; return 0; }
    std::vector<uint8_t> request;
    request.insert(request.end(), { 'D', 'X', 'C', 'Q' });
    put32(request, flags);
    uint32_t define_count = 0;
    if (defines) while (R32(defines + define_count * 8) && define_count < 1024) define_count++;
    put32(request, define_count);
    putz(request, entry); putz(request, profile);
    // The macros go into the request sorted by name: the game builds its macro
    // list by switching entries on and off as graphics settings change
    // (image 0x007CD2B0), so the same set arrives in different orders. For
    // these plain flag macros the order does not change the compiled shader;
    // sorting makes one cache entry serve every order (tools/shader_variants.py
    // precompiles the sets the settings can produce).
    std::vector<std::pair<std::string, std::string>> macros;
    for (uint32_t i = 0; i < define_count; i++) {
        const char *name = guest_string(R32(defines + i * 8), 1024), *value = guest_string(R32(defines + i * 8 + 4), 65536);
        if (!name || !value) return stop("D3DXCompileShader: macro string");
        macros.emplace_back(name, value);
    }
    std::stable_sort(macros.begin(), macros.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
    for (const auto &m : macros) { putz(request, m.first.c_str()); putz(request, m.second.c_str()); }
    put32(request, length);
    request.insert(request.end(), g_base + source, g_base + source + length);
    Sha256 sha; sha.update(request.data(), request.size());
    std::string key = sha.hex();
    if (g_cache_dir.empty()) return stop("D3DXCompileShader: no shader cache directory (PORT_SHADER_CACHE)");
    std::string base = g_cache_dir + "/" + key;
    std::vector<uint8_t> response;
    const char *origin = "cache";
    std::lock_guard<std::mutex> guard(g_compile_lock);
    if (!read_file(base + ".dxcr", &response) || response.size() < 16) {
        if (g_helper.empty()) {
            write_file(base + ".dxcq", request);           // what a host run has to compile
            LOGF("D3DX shader %s (%s %s, %u bytes, %u defines) is not in the cache %s", key.c_str(), profile, entry, (unsigned)length, (unsigned)define_count, g_cache_dir.c_str());
            return stop("D3DXCompileShader: shader not in cache");
        }
        if (!write_file(base + ".dxcq", request)) return stop("D3DXCompileShader: cannot write request");
        std::string command = g_helper + " '" + base + ".dxcq' '" + base + ".dxcr'";
        int status = system(command.c_str());
        if (status || !read_file(base + ".dxcr", &response) || response.size() < 16) {
            LOGF("D3DX helper failed (status %d): %s", status, command.c_str());
            return stop("D3DXCompileShader: compiler helper failed");
        }
        origin = "compiled";
    }
    uint32_t header[4]; memcpy(header, response.data(), 16);
    if (memcmp(response.data(), "DXCR", 4) || (uint64_t)16 + header[2] + header[3] > response.size())
        return stop("D3DXCompileShader: damaged cache entry");
    const uint8_t *shader = response.data() + 16, *messages = shader + header[2];
    static int trace = -1;
    if (trace < 0) trace = getenv("PORT_TRACE_D3D") ? 1 : 0;
    if (trace || header[1]) LOGF("D3DX compile %s %s len=%u defines=%u flags=0x%X -> hr=0x%08X bytes=%u messages=%u (%s %.12s)", profile, entry,
                                 (unsigned)length, (unsigned)define_count, (unsigned)flags, (unsigned)header[1], (unsigned)header[2], (unsigned)header[3], origin, key.c_str());
    if (a[7]) W32(a[7], header[2] ? make_buffer(shader, header[2]) : 0);
    if (a[8]) W32(a[8], header[3] ? make_buffer(messages, header[3]) : 0);
    if (a[9]) {
        const uint8_t *table = nullptr; uint32_t size = 0;
        uint32_t object = (header[2] && find_ctab(shader, header[2], &table, &size)) ? make_table(table, size) : 0;
        W32(a[9], object);
    }
    *r = header[1];
    return 0;
}

int get_constant_table(const uint32_t *a, uint32_t *r) {    // (pFunction, ppConstantTable)
    if (!a[0] || !a[1]) { *r = D3DERR_INVALIDCALL_; return 0; }
    const uint8_t *code = g_base + a[0];
    size_t length = shader_length(code, 4u << 20);
    const uint8_t *table = nullptr; uint32_t size = 0;
    W32(a[1], 0);
    if (!length || !find_ctab(code, length, &table, &size)) { *r = 0x88760B59u; return 0; }   // D3DXERR_INVALIDDATA
    uint32_t object = make_table(table, size);
    if (!object) { *r = 0x88760B59u; return 0; }
    W32(a[1], object);
    *r = S_OK_;
    return 0;
}

// ------------------------------------------------------------------ methods
int call_method(Object *object, uint32_t self, uint32_t slot, const uint32_t *a, uint32_t *r) {
    switch (slot) {
    case 0: {                                               // QueryInterface(riid, ppv)
        static const uint8_t unknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
        // IID_ID3DXBuffer {8BA5FB08-5195-40E2-AC58-0D989C3A0102}, IID_ID3DXConstantTable {AB3C758F-093E-4356-B762-4DB18F1B3A01}
        static const uint8_t buffer[16] = { 0x08, 0xFB, 0xA5, 0x8B, 0x95, 0x51, 0xE2, 0x40, 0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02 };
        static const uint8_t table[16] = { 0x8F, 0x75, 0x3C, 0xAB, 0x3E, 0x09, 0x56, 0x43, 0xB7, 0x62, 0x4D, 0xB1, 0x8F, 0x1B, 0x3A, 0x01 };
        if (!a[1] || !a[2]) { *r = 0x80004003u; return 0; }
        if (!memcmp(g_base + a[1], unknown, 16) || !memcmp(g_base + a[1], buffer, 16) ||
            (object->iface == IF_TABLE && !memcmp(g_base + a[1], table, 16))) { object->refs++; W32(a[2], self); *r = S_OK_; return 0; }
        W32(a[2], 0); *r = E_NOINTERFACE_;
        return 0;
    }
    case 1: *r = ++object->refs; return 0;
    case 2: {
        uint32_t left = --object->refs;
        *r = left;
        if (!left) {
            { std::lock_guard<std::mutex> guard(g_lock); g_objects.erase(self); }
            d3d9t_guest_free(object->data); d3d9t_guest_free(object->handles); d3d9t_guest_free(self);
            delete object;
        }
        return 0;
    }
    case 3: *r = object->data; return 0;
    case 4: *r = object->size; return 0;
    default: break;
    }
    if (object->iface != IF_TABLE) return stop("bad slot");
    switch (slot) {
    case 5:                                                 // GetDesc(pDesc)
        if (!a[1]) { *r = D3DERR_INVALIDCALL_; return 0; }
        W32(a[1], object->data + object->creator); W32(a[1] + 4, object->version); W32(a[1] + 8, (uint32_t)object->constants.size());
        *r = S_OK_;
        return 0;
    case 6: {                                               // GetConstantDesc(hConstant, pDesc, pCount)
        int index;
        if (resolve(object, a[1], &index)) return -1;
        if (index < 0) { *r = D3DERR_INVALIDCALL_; return 0; }
        const Constant &c = object->constants[(size_t)index];
        if (c.members) return stop("constant-table structure constant");
        if (a[2]) {
            uint32_t d = a[2];
            W32(d, object->data + c.name); W32(d + 4, c.register_set); W32(d + 8, c.register_index); W32(d + 12, c.register_count);
            W32(d + 16, c.cls); W32(d + 20, c.type); W32(d + 24, c.rows); W32(d + 28, c.columns); W32(d + 32, c.elements);
            W32(d + 36, c.members); W32(d + 40, 4u * c.elements * c.rows * c.columns);
            W32(d + 44, c.default_value ? object->data + c.default_value : 0);
        }
        if (a[3]) W32(a[3], 1);
        *r = S_OK_;
        return 0;
    }
    case 7: {                                               // GetSamplerIndex(hConstant)
        int index;
        if (resolve(object, a[1], &index)) return -1;
        *r = (index >= 0 && object->constants[(size_t)index].register_set == 3) ? object->constants[(size_t)index].register_index : 0xFFFFFFFFu;
        return 0;
    }
    case 8:                                                 // GetConstant(hConstant, Index)
        if (a[1]) return stop("constant-table GetConstant below a parent");
        *r = a[2] < object->constants.size() ? object->handles + a[2] * 4 : 0;
        return 0;
    case 9: {                                               // GetConstantByName(hConstant, pName)
        if (a[1]) return stop("constant-table GetConstantByName below a parent");
        int index;
        if (resolve(object, a[2], &index)) return -1;
        *r = index >= 0 ? object->handles + (uint32_t)index * 4 : 0;
        return 0;
    }
    default:
        return stop("constant-table method not implemented");
    }
}

}  // namespace

extern "C" void d3dxt_init(uint8_t *flat, const char *cache_dir) {
    g_base = flat;
    for (uint32_t slot = 0; slot < sizeof kBufferMethods / sizeof *kBufferMethods; slot++) W32(kVtableBase + slot * 4, kTrapBase + slot * 8);
    for (uint32_t slot = 0; slot < sizeof kTableMethods / sizeof *kTableMethods; slot++) W32(kVtableBase + 0x400 + slot * 4, kTrapBase + 0x1000 + slot * 8);
    const char *env = getenv("PORT_SHADER_CACHE");
    g_cache_dir = env ? env : (cache_dir ? cache_dir : "");
    if (!g_cache_dir.empty()) mkdir(g_cache_dir.c_str(), 0755);
    env = getenv("PORT_D3DX_HELPER");
    g_helper = env ? env : "";
    LOGF("D3DX shader cache: %s%s", g_cache_dir.empty() ? "(none)" : g_cache_dir.c_str(), g_helper.empty() ? "" : " (+ compiler helper)");
}

extern "C" int d3dxt_compile_shader(const uint32_t *args, uint32_t *result) { t_stop = ""; return compile_shader(args, result); }
extern "C" int d3dxt_get_shader_constant_table(const uint32_t *args, uint32_t *result) { t_stop = ""; return get_constant_table(args, result); }
extern "C" const char *d3dxt_last_stop(void) { return t_stop; }

extern "C" const char *d3dxt_trap_name(uint32_t trap) {
    static thread_local char text[96];
    uint32_t iface = (trap - kTrapBase) >> 12, slot = ((trap - kTrapBase) & 0xFFFu) / 8u;
    const char *method = "?";
    if (iface == IF_BUFFER && slot < sizeof kBufferMethods / sizeof *kBufferMethods) method = kBufferMethods[slot];
    if (iface == IF_TABLE && slot < sizeof kTableMethods / sizeof *kTableMethods) method = kTableMethods[slot];
    snprintf(text, sizeof text, "%s::%s", iface == IF_BUFFER ? "ID3DXBuffer" : "ID3DXConstantTable", method);
    return text;
}

extern "C" int d3dxt_call(uint32_t trap, const uint32_t *args, uint32_t *eax) {
    uint32_t iface = (trap - kTrapBase) >> 12, slot = ((trap - kTrapBase) & 0xFFFu) / 8u;
    uint32_t count = iface == IF_BUFFER ? sizeof kBufferMethods / sizeof *kBufferMethods : sizeof kTableMethods / sizeof *kTableMethods;
    t_stop = "";
    if (iface > IF_TABLE || slot >= count || ((trap - kTrapBase) & 7u)) { t_stop = "bad-trap-address"; return -1; }
    Object *object = object_of(args[0]);
    if (!object || object->iface != (int)iface) { t_stop = "unknown-this"; return -1; }
    if (call_method(object, args[0], slot, args, eax)) {
        snprintf(t_stop_text, sizeof t_stop_text, "%s: %s", d3dxt_trap_name(trap), t_stop);
        t_stop = t_stop_text;
        return -1;
    }
    return (int)((iface == IF_BUFFER ? kBufferArgs[slot] : kTableArgs[slot]) * 4u);
}
