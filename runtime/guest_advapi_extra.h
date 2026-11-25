#ifndef BURNOUT_GUEST_ADVAPI_EXTRA_H
#define BURNOUT_GUEST_ADVAPI_EXTRA_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_bridge_abi.h"
#include "guest_locale.h"
#include "guest_registry.h"

/* advapi32 write overlay + crypto bridges.
 *
 * The read-only registry projection in runtime/guest_registry.c stays the
 * unchanged layer underneath. This module adds a *writable overlay*: keys that
 * the guest creates or opens for writing live in memory and are persisted as
 * JSON in <user_root>/registry-overlay.json. A read through an overlay handle
 * consults the overlay first and falls through to the projection, so the
 * captured Origin/EA values keep being served and are never rewritten.
 *
 * Modelled:
 *   - RegOpenKeyExA / RegCreateKeyExA / RegSetValueExA / RegDeleteKeyA /
 *     RegEnumKeyExA / RegQueryInfoKeyW over that overlay. ANSI names are CP1252
 *     and are converted with the installed GuestLocale; ANSI string values are
 *     stored as UTF-16LE, exactly like the documented A-to-W conversion.
 *   - RegQueryValueExA: overlay value, else the projection through a guest
 *     scratch buffer, converted back to CP1252.
 *   - CryptAcquireContextA: a provider object for CRYPT_VERIFYCONTEXT (no key
 *     container exists in this runtime, so a container request returns the
 *     documented NTE_BAD_KEYSET instead of inventing a key store).
 *   - CryptGenRandom: real bytes from getrandom(2) (fallback /dev/urandom).
 *   - CryptReleaseContext.
 *
 * Documented limits (all named stops or documented Win32 errors):
 *   - REG_OPTION_VOLATILE / CREATE_LINK / BACKUP_RESTORE, non-NULL security
 *     attributes and key classes are not modelled
 *   - RegEnumKeyExA enumerates overlay subkeys only; the read-only projection
 *     cannot be enumerated (no enumeration data was captured)
 *   - deleting a key that exists only in the projection is ERROR_ACCESS_DENIED
 *   - the overlay is process-local; it is not a Windows hive
 *
 * Guest memory is only touched through gm_check/gm_read/gm_write. A non-zero
 * function return is a named stop, never a Win32 status. */

#define GAX_ERROR_SUCCESS 0u
#define GAX_ERROR_FILE_NOT_FOUND 2u
#define GAX_ERROR_PATH_NOT_FOUND 3u
#define GAX_ERROR_ACCESS_DENIED 5u
#define GAX_ERROR_INVALID_HANDLE 6u
#define GAX_ERROR_NOT_ENOUGH_MEMORY 8u
#define GAX_ERROR_INVALID_PARAMETER 87u
#define GAX_ERROR_MORE_DATA 234u
#define GAX_ERROR_NO_MORE_ITEMS 259u
#define GAX_ERROR_INVALID_NAME 123u

#define GAX_REG_OPTION_NON_VOLATILE 0u
#define GAX_REG_OPTION_VOLATILE 1u
#define GAX_REG_OPTION_CREATE_LINK 2u
#define GAX_REG_OPTION_BACKUP_RESTORE 4u
#define GAX_REG_CREATED_NEW_KEY 1u
#define GAX_REG_OPENED_EXISTING_KEY 2u

#define GAX_REG_NONE 0u
#define GAX_REG_SZ 1u
#define GAX_REG_EXPAND_SZ 2u
#define GAX_REG_BINARY 3u
#define GAX_REG_DWORD 4u
#define GAX_REG_DWORD_BIG_ENDIAN 5u
#define GAX_REG_LINK 6u
#define GAX_REG_MULTI_SZ 7u
#define GAX_REG_QWORD 11u

#define GAX_CRYPT_VERIFYCONTEXT 0xf0000000u
#define GAX_CRYPT_NEWKEYSET 0x00000008u
#define GAX_CRYPT_DELETEKEYSET 0x00000010u
#define GAX_CRYPT_MACHINE_KEYSET 0x00000020u
#define GAX_CRYPT_SILENT 0x00000040u
#define GAX_PROV_RSA_FULL 1u
#define GAX_PROV_RSA_SIG 2u
#define GAX_PROV_DSS 3u
#define GAX_PROV_RSA_SCHANNEL 12u
#define GAX_PROV_DH_SCHANNEL 18u
#define GAX_PROV_RSA_AES 24u
#define GAX_NTE_BAD_KEYSET 0x80090016u
#define GAX_NTE_BAD_PROV_TYPE 0x80090014u
#define GAX_NTE_BAD_FLAGS 0x80090009u
#define GAX_NTE_KEYSET_NOT_DEF 0x80090019u

/* Scratch layout inside the guest buffer handed to gax_set_scratch(). */
#define GAX_SCRATCH_NAME 0u
#define GAX_SCRATCH_NAME_SIZE 1024u
#define GAX_SCRATCH_DATA 1024u
#define GAX_SCRATCH_DATA_SIZE 8192u
#define GAX_SCRATCH_TYPE (GAX_SCRATCH_DATA + GAX_SCRATCH_DATA_SIZE)
#define GAX_SCRATCH_SIZE (GAX_SCRATCH_TYPE + 8u)
#define GAX_SCRATCH_MIN 16384u

/* Module-local named stops. */
enum {
    GU_STOP_GAX_NO_SCRATCH = -2401,
    GU_STOP_GAX_NO_LOCALE = -2402,
    GU_STOP_GAX_OPTIONS = -2403,
    GU_STOP_GAX_SECURITY = -2404,
    GU_STOP_GAX_VALUE_TYPE = -2405,
    GU_STOP_GAX_TOO_LARGE = -2406,
    GU_STOP_GAX_BAD_ARGS = -2407,
    GU_STOP_GAX_PERSIST = -2408,
    GU_STOP_GAX_PROVIDER = -2409
};

typedef struct GuestAdvapiExtra GuestAdvapiExtra;

/* The projection and locale are borrowed. user_root is the writable host
 * directory for the JSON overlay (the same root the path map uses). */
int gax_create(GuestMemory *, uint32_t teb, GuestRegistry *,
               const GuestLocale *, const char *user_root,
               GuestAdvapiExtra **out);
void gax_destroy(GuestAdvapiExtra *);
/* Guest scratch for ANSI/UTF-8 conversion and projection queries. */
int gax_set_scratch(GuestAdvapiExtra *, uint32_t address, uint32_t size);
/* RegCloseKey on an overlay key handle: 1 = closed here, 0 = not ours. */
int gax_reg_close_key_if_overlay(GuestAdvapiExtra *, uint32_t handle, uint32_t *result);
/* Host side: put a REG_SZ value (ASCII) into the overlay, e.g. the setting an
 * installer would have written. `path` is "HKLM\\Software\\..." style. */
int gax_install_string(GuestAdvapiExtra *, const char *path, const char *name,
                       const char *ascii);

/* Diagnostics. */
uint32_t gax_overlay_keys(const GuestAdvapiExtra *);
uint32_t gax_overlay_values(const GuestAdvapiExtra *);
uint32_t gax_persist_writes(const GuestAdvapiExtra *);
uint32_t gax_providers(const GuestAdvapiExtra *);
uint32_t gax_random_bytes(const GuestAdvapiExtra *);
const char *gax_overlay_path(const GuestAdvapiExtra *);
const char *gax_last_stop(const GuestAdvapiExtra *);
const char *gax_stop_name(int code);

/* --- advapi32 ---------------------------------------------------------- */
int gax_reg_open_key_ex_a(GuestAdvapiExtra *, const uint32_t *args,
                          uint32_t *result);
int gax_reg_query_value_ex_a(GuestAdvapiExtra *, const uint32_t *args,
                             uint32_t *result);
int gax_reg_create_key_ex_a(GuestAdvapiExtra *, const uint32_t *args,
                            uint32_t *result);
int gax_reg_set_value_ex_a(GuestAdvapiExtra *, const uint32_t *args,
                           uint32_t *result);
int gax_reg_delete_key_a(GuestAdvapiExtra *, const uint32_t *args,
                         uint32_t *result);
int gax_reg_enum_key_ex_a(GuestAdvapiExtra *, const uint32_t *args,
                          uint32_t *result);
int gax_reg_query_info_key_w(GuestAdvapiExtra *, const uint32_t *args,
                             uint32_t *result);
int gax_crypt_acquire_context_a(GuestAdvapiExtra *, const uint32_t *args,
                                uint32_t *result);
int gax_crypt_gen_random(GuestAdvapiExtra *, const uint32_t *args,
                         uint32_t *result);
int gax_crypt_release_context(GuestAdvapiExtra *, const uint32_t *args,
                              uint32_t *result);

/* Registration list for runtime/win32_api_table.c (wired later). */
typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestAdvapiExtra *, const uint32_t *args, uint32_t *result);
} GuestAdvapiExtraExport;

extern const GuestAdvapiExtraExport advapi32_extra_exports[];
extern const uint32_t advapi32_extra_export_count;

#endif
