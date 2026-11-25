/* d3dxcc.exe: compile one HLSL shader with the user's own d3dx9_37.dll.
 *
 * Development/host tool (runs under Wine on the build host): the port's
 * D3DXCompileShader bridge hands the game's exact arguments to this program
 * and stores the result in the private shader cache, so the bytecode is the
 * one Microsoft's compiler produces -- not a re-implementation's.
 *
 *   d3dxcc.exe <request-file> <response-file>
 *
 * request : "DXCQ" u32 flags, u32 defines, zstr entry, zstr profile,
 *           defines x (zstr name, zstr definition), u32 length, source bytes
 * response: "DXCR" u32 hresult, u32 shader bytes, u32 message bytes, shader, message
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *Name, *Definition; } Macro;
typedef struct Buffer Buffer;
struct BufferVtbl {
    HRESULT (WINAPI *QueryInterface)(Buffer *, const void *, void **);
    ULONG (WINAPI *AddRef)(Buffer *);
    ULONG (WINAPI *Release)(Buffer *);
    void *(WINAPI *GetBufferPointer)(Buffer *);
    DWORD (WINAPI *GetBufferSize)(Buffer *);
};
struct Buffer { const struct BufferVtbl *vtbl; };
typedef HRESULT (WINAPI *CompileFn)(const char *, UINT, const Macro *, void *, const char *, const char *, DWORD,
                                    Buffer **, Buffer **, void **);

static unsigned char *g_data; static size_t g_size, g_pos;
static unsigned u32(void) { unsigned v = 0; if (g_pos + 4 <= g_size) memcpy(&v, g_data + g_pos, 4); g_pos += 4; return v; }
static const char *zstr(void) {
    const char *s = (const char *)g_data + g_pos;
    while (g_pos < g_size && g_data[g_pos]) g_pos++;
    g_pos++;
    return s;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: d3dxcc <request> <response>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "d3dxcc: cannot read %s\n", argv[1]); return 3; }
    fseek(f, 0, SEEK_END); g_size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_data = malloc(g_size + 1);
    if (fread(g_data, 1, g_size, f) != g_size) return 3;
    fclose(f);
    if (g_size < 16 || memcmp(g_data, "DXCQ", 4)) { fprintf(stderr, "d3dxcc: bad request\n"); return 4; }
    g_pos = 4;
    unsigned flags = u32(), count = u32();
    const char *entry = zstr(), *profile = zstr();
    Macro *macros = calloc(count + 1, sizeof *macros);
    for (unsigned i = 0; i < count; i++) { macros[i].Name = zstr(); macros[i].Definition = zstr(); }
    unsigned length = u32();
    if (g_pos + length > g_size) { fprintf(stderr, "d3dxcc: truncated request\n"); return 4; }
    const char *source = (const char *)g_data + g_pos;

    HMODULE dll = LoadLibraryA("d3dx9_37.dll");
    if (!dll) { fprintf(stderr, "d3dxcc: cannot load d3dx9_37.dll (%lu)\n", GetLastError()); return 5; }
    CompileFn compile = (CompileFn)GetProcAddress(dll, "D3DXCompileShader");
    if (!compile) { fprintf(stderr, "d3dxcc: no D3DXCompileShader\n"); return 5; }
    Buffer *shader = NULL, *messages = NULL; void *table = NULL;
    HRESULT hr = compile(source, length, count ? macros : NULL, NULL, *entry ? entry : NULL, profile, flags,
                         &shader, &messages, &table);
    unsigned shader_size = shader ? shader->vtbl->GetBufferSize(shader) : 0;
    unsigned message_size = messages ? messages->vtbl->GetBufferSize(messages) : 0;
    f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "d3dxcc: cannot write %s\n", argv[2]); return 6; }
    unsigned header[4]; memcpy(header, "DXCR", 4); header[1] = (unsigned)hr; header[2] = shader_size; header[3] = message_size;
    fwrite(header, 1, sizeof header, f);
    if (shader_size) fwrite(shader->vtbl->GetBufferPointer(shader), 1, shader_size, f);
    if (message_size) fwrite(messages->vtbl->GetBufferPointer(messages), 1, message_size, f);
    fclose(f);
    return 0;
}
