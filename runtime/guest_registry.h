#ifndef BURNOUT_GUEST_REGISTRY_H
#define BURNOUT_GUEST_REGISTRY_H
#include "guest_memory.h"
/* Read-only registry projection for the port-owned guest process.
 *
 * Only fields captured read-only from the compatibility prefix are served
 * ((development evidence, not published) proton-version-profile.json,
 * proton-locale-profile.json). Every other key or value reports
 * ERROR_FILE_NOT_FOUND instead of an invented value; write access stops the
 * runtime explicitly. The value bytes are embedded in the image, never host
 * pointers, and the guest sees only checked 32-bit guest addresses.
 *
 * Each bridge reports the Win32 return value through *status and mirrors the
 * Python model (tools/guest_registry.py) exactly, LastError included. A
 * non-zero function result is a model/guest-memory stop (GM_*), not a Win32
 * status. */
#define GWR_HKEY_CLASSES_ROOT 0x80000000u
#define GWR_HKEY_CURRENT_USER 0x80000001u
#define GWR_HKEY_LOCAL_MACHINE 0x80000002u
#define GWR_HKEY_USERS 0x80000003u
#define GWR_REG_SZ 1u
#define GWR_REG_EXPAND_SZ 2u
#define GWR_REG_BINARY 3u
#define GWR_REG_DWORD 4u
#define GWR_ERROR_SUCCESS 0u
#define GWR_ERROR_FILE_NOT_FOUND 2u
#define GWR_ERROR_MORE_DATA 234u
/* KEY_SET_VALUE|KEY_CREATE_SUB_KEY|KEY_CREATE_LINK|WRITE_DAC|WRITE_OWNER;
 * ENUMERATE_SUB_KEYS and NOTIFY are read access. */
#define GWR_WRITE_ACCESS (0x0002u|0x0004u|0x0020u|0x40000u|0x80000u)
#define GWR_HANDLE_BASE 0x0f000001u
/* Table entries. The projection itself is generated from the Python model by
 * port/build/agents/a3-generate-registry-table.py. */
typedef struct { const char *name; uint32_t type; const uint8_t *data; uint32_t size; } GuestRegistryValue;
typedef struct { uint32_t root; const char *path; uint32_t first_value; uint32_t value_count; } GuestRegistryKey;
typedef struct {
    uint32_t key_count,value_count;
    const GuestRegistryKey *keys;
    const GuestRegistryValue *values;
} GuestRegistryLayout;
extern const GuestRegistryLayout guest_registry_layout;
typedef struct GuestRegistry GuestRegistry;
int gwr_create(GuestRegistry **out);
void gwr_destroy(GuestRegistry *);
uint32_t gwr_queries(const GuestRegistry *);
/* Requests that the projection could not serve, in arrival order. */
uint32_t gwr_unserved_keys(const GuestRegistry *);
uint32_t gwr_unserved_values(const GuestRegistry *);
const char *gwr_unserved_key(const GuestRegistry *,uint32_t index);
const char *gwr_unserved_value(const GuestRegistry *,uint32_t index);
/* RegOpenKeyExW: read-only projection, WOW64 redirection for HKLM\Software. */
int gwr_open(GuestRegistry *,GuestMemory *,uint32_t teb,uint32_t key,
             uint32_t subkey,uint32_t options,uint32_t access,
             uint32_t result,uint32_t *status);
/* RegQueryValueExW: size query, exact copy, ERROR_MORE_DATA on short buffer. */
int gwr_query(GuestRegistry *,GuestMemory *,uint32_t teb,uint32_t handle,
              uint32_t name,uint32_t reserved,uint32_t type,uint32_t data,
              uint32_t size,uint32_t *status);
/* RegCloseKey: predefined roots and handles issued by this projection only. */
int gwr_close(GuestRegistry *,GuestMemory *,uint32_t teb,uint32_t handle,
              uint32_t *status);
#endif
