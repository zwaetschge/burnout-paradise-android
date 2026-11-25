/* shell32 / gdi32 / imm32 / powrprof / dbghelp bridges.
 * See guest_shell_gdi.h for the modelled scope and the named limits. */
#include "guest_shell_gdi.h"
#include "guest_handles.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define GSG_OBJECT_MAGIC 0x4f534747u /* "GGSO" */

typedef struct {
    uint32_t magic;   /* GSG_OBJECT_MAGIC: the handle table is shared */
    int kind;         /* 1 = memory DC, 2 = bitmap */
    uint32_t width, height;
    uint32_t *bits;   /* host-side 32-bpp pixels, never guest visible */
    uint32_t selected;/* bitmap handle selected into a memory DC */
} GsgObject;

struct GuestShellGdi {
    GuestMemory *memory;
    uint32_t teb;
    GuestPathMap *map;
    const GuestLocale *locale;
    uint32_t surface_width, surface_height;
    uint32_t screen_bitblt_calls;
    uint32_t unknown_device_caps;
    uint32_t shell_execute_calls;
    uint32_t dbghelp_calls;
    char last_log[192];
};

static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static int write_u32(GuestShellGdi *sg, uint32_t address, uint32_t value) {
    uint8_t b[4];
    put32(b, value);
    return gm_write(sg->memory, address, b, 4);
}
static int last_error(GuestShellGdi *sg, uint32_t value) {
    return write_u32(sg, sg->teb + 0x34, value);
}
static int stop(GuestShellGdi *sg, int code, const char *what) {
    snprintf(sg->last_log, sizeof(sg->last_log), "%s", what);
    fprintf(stderr, "shell-gdi-stop: %s (%d)\n", what, code);
    return code;
}
static void note(GuestShellGdi *sg, const char *what) {
    snprintf(sg->last_log, sizeof(sg->last_log), "%s", what);
}

int gsg_create(GuestMemory *memory, uint32_t teb, GuestPathMap *map,
               const GuestLocale *locale, GuestShellGdi **out) {
    if (!memory || !map || !out) return GM_RANGE;
    GuestShellGdi *sg = (GuestShellGdi *)calloc(1, sizeof(*sg));
    if (!sg) return GM_ALLOCATION;
    sg->memory = memory;
    sg->teb = teb;
    sg->map = map;
    sg->locale = locale;
    sg->surface_width = 1280;
    sg->surface_height = 720;
    *out = sg;
    return GM_OK;
}
void gsg_destroy(GuestShellGdi *sg) { free(sg); }

int gsg_set_surface_size(GuestShellGdi *sg, uint32_t width, uint32_t height) {
    if (!sg || !width || !height) return GM_UNSUPPORTED;
    sg->surface_width = width;
    sg->surface_height = height;
    return GM_OK;
}
uint32_t gsg_surface_width(const GuestShellGdi *sg) {
    return sg ? sg->surface_width : 0u;
}
uint32_t gsg_surface_height(const GuestShellGdi *sg) {
    return sg ? sg->surface_height : 0u;
}
uint32_t gsg_screen_bitblt_calls(const GuestShellGdi *sg) {
    return sg ? sg->screen_bitblt_calls : 0u;
}
uint32_t gsg_unknown_device_caps(const GuestShellGdi *sg) {
    return sg ? sg->unknown_device_caps : 0u;
}
uint32_t gsg_shell_execute_calls(const GuestShellGdi *sg) {
    return sg ? sg->shell_execute_calls : 0u;
}
uint32_t gsg_dbghelp_calls(const GuestShellGdi *sg) {
    return sg ? sg->dbghelp_calls : 0u;
}
const char *gsg_last_log(const GuestShellGdi *sg) {
    return (sg && sg->last_log[0]) ? sg->last_log : NULL;
}
const char *gsg_stop_name(int code) {
    switch (code) {
    case 0: return "ok";
    case 1: return "gm-range";
    case 2: return "gm-uncommitted";
    case 3: return "gm-allocation";
    case 4: return "gm-access";
    case 5: return "gm-unsupported";
    case 6: return "gm-overlap";
    case GU_STOP_GSG_NO_LOCALE: return "shell-ansi-without-code-page";
    case GU_STOP_GSG_NON_BMP: return "shell-non-bmp-guest-string";
    case GU_STOP_GSG_PATH_TOO_LONG: return "shell-guest-path-too-long";
    case GU_STOP_GSG_BAD_HANDLE: return "gdi-handle-not-issued-here";
    case GU_STOP_GSG_ROP: return "gdi-bitblt-rop-unsupported";
    case GU_STOP_GSG_NO_BITMAP: return "gdi-memory-dc-without-bitmap";
    case GU_STOP_GSG_BAD_ARGS: return "shell-gdi-argument-outside-model";
    case GU_STOP_GSG_SECURITY_ATTR: return "shell-security-attributes-unsupported";
    default: return "unknown-stop";
    }
}

/* --- guest string helpers ---------------------------------------------- */
static int utf8_decode(const char *text, uint32_t *code, uint32_t *used) {
    const uint8_t *s = (const uint8_t *)text;
    if (s[0] < 0x80) { *code = s[0]; *used = 1; return 0; }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
        *code = ((uint32_t)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
        *used = 2;
        return 0;
    }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        *code = ((uint32_t)(s[0] & 0x0f) << 12) |
                ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        *used = 3;
        return 0;
    }
    return GM_UNSUPPORTED;
}
/* Writes a NUL-terminated UTF-16 copy; capacity is in code units. */
static int write_utf16(GuestMemory *m, uint32_t address, const char *text,
                       uint32_t capacity) {
    uint32_t pos = 0, units = 0;
    while (text[pos]) {
        uint32_t code, used;
        int rc = utf8_decode(text + pos, &code, &used);
        if (rc) return rc;
        if (code > 0xffff) return GM_UNSUPPORTED;
        if (units + 1 >= capacity) return GM_UNSUPPORTED;
        uint8_t bytes[2] = {(uint8_t)code, (uint8_t)(code >> 8)};
        rc = gm_write(m, address + units * 2u, bytes, 2);
        if (rc) return rc;
        units++;
        pos += used;
    }
    uint8_t nul[2] = {0, 0};
    return gm_write(m, address + units * 2u, nul, 2);
}
/* Writes a NUL-terminated CP1252 copy; capacity is in bytes. */
static int write_ansi(GuestShellGdi *sg, uint32_t address, const char *text,
                      uint32_t capacity) {
    if (!sg->locale || !sg->locale->installed) return GU_STOP_GSG_NO_LOCALE;
    uint32_t pos = 0, len = 0;
    uint8_t buffer[GSG_MAX_PATH];
    while (text[pos]) {
        uint32_t code, used;
        int rc = utf8_decode(text + pos, &code, &used);
        if (rc) return rc;
        if (len + 1 >= capacity || len + 1 >= sizeof(buffer))
            return GU_STOP_GSG_PATH_TOO_LONG;
        uint8_t byte = (code < 0x10000) ? sg->locale->encode[code] : 0;
        if (!byte) byte = '?';
        buffer[len++] = byte;
        pos += used;
    }
    buffer[len] = 0;
    return gm_write(sg->memory, address, buffer, len + 1);
}
/* Reads a NUL-terminated ANSI guest string into UTF-8. */
static int read_ansi(GuestShellGdi *sg, uint32_t address, char *out,
                     uint32_t capacity) {
    if (!sg->locale || !sg->locale->installed) return GU_STOP_GSG_NO_LOCALE;
    uint32_t len = 0;
    for (uint32_t i = 0; i < GSG_MAX_GUEST_PATH; i++) {
        uint8_t byte;
        int rc = gm_read(sg->memory, address + i, &byte, 1);
        if (rc) return rc;
        if (!byte) break;
        uint32_t code = 0;
        if (gl_decode_byte(sg->locale, byte, &code)) return GM_UNSUPPORTED;
        if (code < 0x80) {
            if (len + 1 >= capacity) return GU_STOP_GSG_PATH_TOO_LONG;
            out[len++] = (char)code;
        } else if (code < 0x800) {
            if (len + 2 >= capacity) return GU_STOP_GSG_PATH_TOO_LONG;
            out[len++] = (char)(0xc0 | (code >> 6));
            out[len++] = (char)(0x80 | (code & 0x3f));
        } else {
            if (len + 3 >= capacity) return GU_STOP_GSG_PATH_TOO_LONG;
            out[len++] = (char)(0xe0 | (code >> 12));
            out[len++] = (char)(0x80 | ((code >> 6) & 0x3f));
            out[len++] = (char)(0x80 | (code & 0x3f));
        }
        if (i + 1 == GSG_MAX_GUEST_PATH) return GU_STOP_GSG_PATH_TOO_LONG;
    }
    out[len] = 0;
    return GM_OK;
}

/* --- CSIDL table -------------------------------------------------------- */
typedef struct {
    uint32_t csidl;
    const char *guest;
} GsgCsidl;

static const GsgCsidl gsg_csidls[] = {
    { GSG_CSIDL_DESKTOP, "C:\\Users\\Player\\Desktop" },
    { GSG_CSIDL_DESKTOPDIRECTORY, "C:\\Users\\Player\\Desktop" },
    { GSG_CSIDL_PERSONAL, "C:\\Users\\Player\\Documents" },
    { GSG_CSIDL_FAVORITES, "C:\\Users\\Player\\Favorites" },
    { GSG_CSIDL_STARTUP, "C:\\Users\\Player\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup" },
    { GSG_CSIDL_SENDTO, "C:\\Users\\Player\\AppData\\Roaming\\Microsoft\\Windows\\SendTo" },
    { GSG_CSIDL_STARTMENU, "C:\\Users\\Player\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu" },
    { GSG_CSIDL_PROGRAMS, "C:\\Users\\Player\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs" },
    { GSG_CSIDL_MYMUSIC, "C:\\Users\\Player\\Music" },
    { GSG_CSIDL_MYVIDEO, "C:\\Users\\Player\\Videos" },
    { GSG_CSIDL_MYPICTURES, "C:\\Users\\Player\\Pictures" },
    { GSG_CSIDL_TEMPLATES, "C:\\Users\\Player\\AppData\\Roaming\\Microsoft\\Windows\\Templates" },
    { GSG_CSIDL_APPDATA, "C:\\Users\\Player\\AppData\\Roaming" },
    { GSG_CSIDL_LOCAL_APPDATA, "C:\\Users\\Player\\AppData\\Local" },
    { GSG_CSIDL_INTERNET_CACHE, "C:\\Users\\Player\\AppData\\Local\\Microsoft\\Windows\\INetCache" },
    { GSG_CSIDL_COOKIES, "C:\\Users\\Player\\AppData\\Local\\Microsoft\\Windows\\INetCookies" },
    { GSG_CSIDL_HISTORY, "C:\\Users\\Player\\AppData\\Local\\Microsoft\\Windows\\History" },
    { GSG_CSIDL_CDBURN_AREA, "C:\\Users\\Player\\AppData\\Local\\Microsoft\\Windows\\CD Burning" },
    { GSG_CSIDL_COMMON_APPDATA, "C:\\ProgramData" },
    { GSG_CSIDL_COMMON_DOCUMENTS, "C:\\Users\\Public\\Documents" },
    { GSG_CSIDL_COMMON_PROGRAMS, "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs" },
    { GSG_CSIDL_COMMON_STARTMENU, "C:\\ProgramData\\Microsoft\\Windows\\Start Menu" },
    { GSG_CSIDL_COMMON_STARTUP, "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup" },
    { GSG_CSIDL_PROFILE, "C:\\Users\\Player" },
    { GSG_CSIDL_WINDOWS, "C:\\Windows" },
    { GSG_CSIDL_SYSTEM, "C:\\Windows\\System32" },
    { GSG_CSIDL_FONTS, "C:\\Windows\\Fonts" },
    { GSG_CSIDL_PROGRAM_FILES, "C:\\Program Files" },
    { GSG_CSIDL_PROGRAM_FILESX86, "C:\\Program Files (x86)" }
};

static const char *csidl_path(uint32_t csidl) {
    uint32_t base = csidl & ~GSG_CSIDL_FLAG_CREATE;
    for (size_t i = 0; i < sizeof(gsg_csidls) / sizeof(gsg_csidls[0]); i++)
        if (gsg_csidls[i].csidl == base) return gsg_csidls[i].guest;
    return NULL;
}

/* Recursive creation through the path projection. */
/* Create a host directory and any missing parents (mkdir -p). The guest path
 * projection skips unmapped intermediates (e.g. ...\AppData with only
 * ...\AppData\Local mapped), so the host parent chain must be filled in here
 * or the deep mkdir fails with ENOENT. */
static int host_mkdir_p(const char *path) {
    char tmp[GPM_MAX_HOST];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof tmp) return (n == 0) ? 0 : -1;
    memcpy(tmp, path, n + 1);
    for (char *q = tmp + 1; *q; q++) {
        if (*q != '/') continue;
        *q = 0;
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) { *q = '/'; return -1; }
        *q = '/';
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}
static int make_directories(GuestShellGdi *sg, const char *guest,
                            uint32_t *error) {
    char canonical[GSG_MAX_GUEST_PATH];
    int rc = gpm_normalize(guest, canonical, sizeof(canonical));
    if (rc) { *error = GSG_ERROR_BAD_PATHNAME; return GM_OK; }
    char host[GPM_MAX_HOST];
    /* Create every mapped prefix in order; unmapped prefixes (C:\Users,
     * C:\Windows, ...) are not part of the projection and are skipped. */
    for (uint32_t i = 2; canonical[i]; i++) {
        if (canonical[i] != '\\') continue;
        canonical[i] = 0;
        rc = gpm_resolve(sg->map, canonical, GPM_MODE_WRITE, host, sizeof(host));
        if (getenv("PORT_DBG_DIR")) fprintf(stderr, "DBG  prefix '%s' rc=%d host='%s'\n", canonical, rc, rc==GPM_OK?host:"");
        canonical[i] = '\\';
        if (rc == GPM_OK) {
            if (host_mkdir_p(host) != 0) {
                *error = (errno == EACCES) ? GSG_ERROR_ACCESS_DENIED
                                           : GSG_ERROR_PATH_NOT_FOUND;
                return GM_OK;
            }
        } else if (rc == GPM_UNMAPPED) {
            continue;
        } else if (rc == GPM_NOT_FOUND || rc == GPM_PATH_NOT_FOUND) {
            *error = GSG_ERROR_PATH_NOT_FOUND;
            return GM_OK;
        } else if (rc == GPM_ACCESS) {
            *error = GSG_ERROR_ACCESS_DENIED;
            return GM_OK;
        } else {
            *error = GSG_ERROR_BAD_PATHNAME;
            return GM_OK;
        }
    }
    rc = gpm_resolve(sg->map, canonical, GPM_MODE_WRITE, host, sizeof(host));
    if (getenv("PORT_DBG_DIR")) fprintf(stderr, "DBG  FINAL '%s' rc=%d host='%s'\n", canonical, rc, rc==GPM_OK?host:"");
    if (rc == GPM_UNMAPPED) { *error = GSG_ERROR_BAD_PATHNAME; return GM_OK; }
    if (rc == GPM_ACCESS) { *error = GSG_ERROR_ACCESS_DENIED; return GM_OK; }
    if (rc == GPM_NOT_FOUND || rc == GPM_PATH_NOT_FOUND) {
        *error = GSG_ERROR_PATH_NOT_FOUND;
        return GM_OK;
    }
    if (rc) { *error = GSG_ERROR_BAD_PATHNAME; return GM_OK; }
    struct stat info;
    if (stat(host, &info) == 0) {
        *error = S_ISDIR(info.st_mode) ? GSG_ERROR_ALREADY_EXISTS
                                       : GSG_ERROR_FILE_EXISTS;
        return GM_OK;
    }
    if (mkdir(host, 0777) != 0) {
        *error = (errno == EACCES) ? GSG_ERROR_ACCESS_DENIED
                                   : GSG_ERROR_PATH_NOT_FOUND;
        return GM_OK;
    }
    *error = GSG_ERROR_SUCCESS;
    return GM_OK;
}

/* --- shell32 ----------------------------------------------------------- */
static int folder_path(GuestShellGdi *sg, const char *guest, uint32_t create) {
    if (!create) return GM_OK;
    uint32_t error = GSG_ERROR_SUCCESS;
    int rc = make_directories(sg, guest, &error);
    if (rc) return rc;
    /* CSIDL_FLAG_CREATE only creates; an existing directory is not an error. */
    return GM_OK;
}
int gsg_sh_get_folder_path_w(GuestShellGdi *sg, const uint32_t *args,
                             uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    uint32_t csidl = args[1];
    uint32_t token = args[2];
    uint32_t flags = args[3];
    if (token) { *result = GSG_E_INVALIDARG; return GM_OK; }
    if (flags != GSG_SHGFP_TYPE_CURRENT && flags != GSG_SHGFP_TYPE_DEFAULT) {
        *result = GSG_E_INVALIDARG;
        return GM_OK;
    }
    const char *guest = csidl_path(csidl);
    if (!guest) { *result = GSG_E_INVALIDARG; return GM_OK; }
    int rc = folder_path(sg, guest, csidl & GSG_CSIDL_FLAG_CREATE);
    if (rc) return rc;
    rc = write_utf16(sg->memory, args[4], guest, GSG_MAX_PATH);
    if (rc == GM_UNSUPPORTED) { *result = GSG_E_INVALIDARG; return GM_OK; }
    if (rc) return rc;
    *result = GSG_S_OK;
    return GM_OK;
}
static int special_folder(GuestShellGdi *sg, const uint32_t *args,
                          uint32_t *result, uint32_t wide) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    const char *guest = csidl_path(args[2]);
    if (!guest) {
        int rc = last_error(sg, GSG_ERROR_INVALID_PARAMETER);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    int rc = folder_path(sg, guest, args[3]);
    if (rc) return rc;
    if (getenv("PORT_DBG_DIR")) fprintf(stderr, "DBG SHGetSpecialFolderPath csidl=%#x guest='%s'\n", args[2], guest);
    rc = wide ? write_utf16(sg->memory, args[1], guest, GSG_MAX_PATH)
              : write_ansi(sg, args[1], guest, GSG_MAX_PATH);
    if (rc == GM_UNSUPPORTED || rc == GU_STOP_GSG_PATH_TOO_LONG) {
        rc = last_error(sg, GSG_ERROR_INVALID_PARAMETER);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}
int gsg_sh_get_special_folder_path_a(GuestShellGdi *sg, const uint32_t *args,
                                     uint32_t *result) {
    return special_folder(sg, args, result, 0);
}
int gsg_sh_get_special_folder_path_w(GuestShellGdi *sg, const uint32_t *args,
                                     uint32_t *result) {
    return special_folder(sg, args, result, 1);
}
int gsg_sh_create_directory_ex_a(GuestShellGdi *sg, const uint32_t *args,
                                 uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    if (args[2]) return stop(sg, GU_STOP_GSG_SECURITY_ATTR,
                             "SHCreateDirectoryExA: lpSecurityAttributes must "
                             "be NULL");
    char guest[GSG_MAX_GUEST_PATH];
    int rc = read_ansi(sg, args[1], guest, sizeof(guest));
    if (rc == GU_STOP_GSG_PATH_TOO_LONG) {
        *result = GSG_ERROR_BAD_PATHNAME;
        return GM_OK;
    }
    if (rc) return rc;
    if (!guest[0]) { *result = GSG_ERROR_BAD_PATHNAME; return GM_OK; }
    uint32_t error = GSG_ERROR_SUCCESS;
    rc = make_directories(sg, guest, &error);
    if (rc) return rc;
    if (getenv("PORT_DBG_DIR")) fprintf(stderr, "DBG SHCreateDirectoryExA guest='%s' -> error=%u\n", guest, error);
    *result = error;
    return GM_OK;
}
/* ShellExecuteA(hwnd, lpOperation, lpFile, lpParameters, lpDirectory, nShow)
 * and ShellExecuteExA(SHELLEXECUTEINFOA*). Neither can start a child process in
 * this runtime, so the documented failure is returned after logging. */
#define GSG_SHELLEXECUTEINFO_A_SIZE 60u
static int shell_execute(GuestShellGdi *sg, const uint32_t *args,
                         uint32_t *result, uint32_t extended) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    sg->shell_execute_calls++;
    char file[GSG_MAX_GUEST_PATH];
    file[0] = 0;
    uint32_t file_argument = args[2];
    uint32_t info = 0;
    int rc;
    if (extended) {
        info = args[0];
        if (!info) return stop(sg, GU_STOP_GSG_BAD_ARGS,
                               "ShellExecuteExA: lpExecInfo must not be NULL");
        uint8_t header[GSG_SHELLEXECUTEINFO_A_SIZE];
        rc = gm_read(sg->memory, info, header, sizeof(header));
        if (rc) return rc;
        uint32_t size = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                        ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
        if (size < GSG_SHELLEXECUTEINFO_A_SIZE) {
            rc = last_error(sg, GSG_ERROR_INVALID_PARAMETER);
            if (rc) return rc;
            *result = 0;
            return GM_OK;
        }
        file_argument = (uint32_t)header[16] | ((uint32_t)header[17] << 8) |
                        ((uint32_t)header[18] << 16) | ((uint32_t)header[19] << 24);
    }
    if (file_argument) {
        rc = read_ansi(sg, file_argument, file, sizeof(file));
        if (rc && rc != GU_STOP_GSG_PATH_TOO_LONG) return rc;
    }
    fprintf(stderr, "shell32: ShellExecute%s(\"%s\") refused: this runtime "
            "cannot start a child process\n", extended ? "Ex" : "", file);
    note(sg, "ShellExecute refused: no child process");
    rc = last_error(sg, GSG_ERROR_ACCESS_DENIED);
    if (rc) return rc;
    if (extended) {
        /* hInstApp carries the SE_ERR_* value on failure. */
        uint8_t value[4];
        put32(value, GSG_SE_ERR_ACCESSDENIED);
        rc = gm_write(sg->memory, info + 32, value, 4);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    *result = GSG_SE_ERR_ACCESSDENIED;
    return GM_OK;
}
int gsg_shell_execute_a(GuestShellGdi *sg, const uint32_t *args,
                        uint32_t *result) {
    return shell_execute(sg, args, result, 0);
}
int gsg_shell_execute_ex_a(GuestShellGdi *sg, const uint32_t *args,
                           uint32_t *result) {
    return shell_execute(sg, args, result, 1);
}

/* --- gdi32 objects ----------------------------------------------------- */
static void gsg_object_destroy(void *object) {
    GsgObject *g = (GsgObject *)object;
    if (!g) return;
    free(g->bits);
    free(g);
}
/* gh_lookup cannot filter by our own sub-kind, so every handle this module
 * issues carries a kind word in the object. */
static GsgObject *gsg_lookup(uint32_t handle) {
    GsgObject *g = (GsgObject *)gh_lookup(handle, GH_KIND_OTHER, NULL);
    if (!g || g->magic != GSG_OBJECT_MAGIC) return NULL;
    if (g->kind != 1 && g->kind != 2) return NULL;
    return g;
}
static int is_dc(uint32_t handle) {
    if (handle == GSG_SCREEN_DC) return 1;
    GsgObject *g = gsg_lookup(handle);
    return g && g->kind == 1;
}
int gsg_get_stock_object(GuestShellGdi *sg, const uint32_t *args,
                         uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    uint32_t index = args[0];
    if (index >= GSG_STOCK_COUNT) {
        int rc = last_error(sg, GSG_ERROR_INVALID_PARAMETER);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    *result = GSG_STOCK_BASE + index;
    return GM_OK;
}
int gsg_create_compatible_dc(GuestShellGdi *sg, const uint32_t *args,
                             uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    if (args[0] && !is_dc(args[0])) {
        int rc = last_error(sg, GSG_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    GsgObject *g = (GsgObject *)calloc(1, sizeof(*g));
    if (!g) return GM_ALLOCATION;
    g->magic = GSG_OBJECT_MAGIC;
    g->kind = 1;
    uint32_t handle = gh_insert(GH_KIND_OTHER, g, gsg_object_destroy);
    if (!handle) { free(g); return GM_ALLOCATION; }
    *result = handle;
    return GM_OK;
}
int gsg_create_compatible_bitmap(GuestShellGdi *sg, const uint32_t *args,
                                 uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    uint32_t width = args[1], height = args[2];
    if (args[0] && !is_dc(args[0])) {
        int rc = last_error(sg, GSG_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    if (!width || !height || width > GSG_MAX_BITMAP_EDGE ||
        height > GSG_MAX_BITMAP_EDGE) {
        int rc = last_error(sg, GSG_ERROR_INVALID_PARAMETER);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    GsgObject *g = (GsgObject *)calloc(1, sizeof(*g));
    if (!g) return GM_ALLOCATION;
    g->magic = GSG_OBJECT_MAGIC;
    g->kind = 2;
    g->width = width;
    g->height = height;
    size_t bytes = (size_t)width * height * 4u;
    g->bits = (uint32_t *)calloc(1, bytes);
    if (!g->bits) { free(g); return GM_ALLOCATION; }
    uint32_t handle = gh_insert(GH_KIND_OTHER, g, gsg_object_destroy);
    if (!handle) { free(g->bits); free(g); return GM_ALLOCATION; }
    *result = handle;
    return GM_OK;
}
int gsg_select_object(GuestShellGdi *sg, const uint32_t *args,
                      uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    GsgObject *dc = gsg_lookup(args[0]);
    if (!dc || dc->kind != 1) {
        int rc = last_error(sg, GSG_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    uint32_t object = args[1];
    uint32_t previous = dc->selected;
    if (object >= GSG_STOCK_BASE && object < GSG_STOCK_BASE + GSG_STOCK_COUNT) {
        /* Stock brushes/pens/fonts are not modelled as DC state. */
        *result = previous;
        return GM_OK;
    }
    GsgObject *bitmap = gsg_lookup(object);
    if (!bitmap || bitmap->kind != 2) {
        int rc = last_error(sg, GSG_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    dc->selected = object;
    dc->width = bitmap->width;
    dc->height = bitmap->height;
    *result = previous;
    return GM_OK;
}
int gsg_delete_object(GuestShellGdi *sg, const uint32_t *args,
                      uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    uint32_t object = args[0];
    if (object >= GSG_STOCK_BASE && object < GSG_STOCK_BASE + GSG_STOCK_COUNT) {
        /* Documented: stock objects must not be deleted; the call is a no-op. */
        *result = 1;
        return GM_OK;
    }
    GsgObject *g = gsg_lookup(object);
    if (!g || g->kind != 2) {
        int rc = last_error(sg, GSG_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    if (!gh_close(object)) return GU_STOP_GSG_BAD_HANDLE;
    *result = 1;
    return GM_OK;
}
int gsg_delete_dc(GuestShellGdi *sg, const uint32_t *args, uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    GsgObject *g = gsg_lookup(args[0]);
    if (!g || g->kind != 1) {
        int rc = last_error(sg, GSG_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    if (!gh_close(args[0])) return GU_STOP_GSG_BAD_HANDLE;
    *result = 1;
    return GM_OK;
}
int gsg_get_device_caps(GuestShellGdi *sg, const uint32_t *args,
                        uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    if (!is_dc(args[0])) {
        int rc = last_error(sg, GSG_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    uint32_t width = sg->surface_width, height = sg->surface_height;
    uint32_t value;
    switch (args[1]) {
    case 2: value = 1; break;                    /* TECHNOLOGY: raster display */
    case 4: value = (width * 254u + 480u) / 960u; break;  /* HORZSIZE (mm) */
    case 6: value = (height * 254u + 480u) / 960u; break; /* VERTSIZE (mm) */
    case 8: value = width; break;                /* HORZRES */
    case 10: value = height; break;              /* VERTRES */
    case 12: value = 32; break;                  /* BITSPIXEL */
    case 14: value = 1; break;                   /* PLANES */
    case 24: value = 0xffffffffu; break;         /* NUMCOLORS: -1 for >8bpp */
    case 28: case 30: case 32: case 34: value = 0; break; /* curve/line/... */
    case 36: value = 1; break;                   /* CLIPCAPS: rectangle */
    case 38: value = 1; break;                   /* RASTERCAPS: RC_BITBLT */
    case 40: case 42: value = 36; break;         /* ASPECTX/ASPECTY */
    case 44: value = 1296; break;                /* ASPECTXY */
    case 88: case 90: value = 96; break;         /* LOGPIXELSX/LOGPIXELSY */
    case 94: value = 0; break;                   /* CAPS1 */
    case 104: value = 0; break;                  /* SIZEPALETTE */
    case 108: value = 24; break;                 /* COLORRES */
    case 120: case 121: value = 0; break;        /* SHADEBLEND/COLORMGMT */
    default:
        sg->unknown_device_caps++;
        value = 0;
        break;
    }
    *result = value;
    return GM_OK;
}
int gsg_get_object_a(GuestShellGdi *sg, const uint32_t *args,
                     uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    GsgObject *g = gsg_lookup(args[0]);
    if (!g || g->kind != 2) {
        int rc = last_error(sg, GSG_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    if (args[1] < 24u || !args[2]) {
        int rc = last_error(sg, GSG_ERROR_INVALID_PARAMETER);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    /* PE32 BITMAP: bmType, bmWidth, bmHeight, bmWidthBytes, bmPlanes,
     * bmBitsPixel, bmBits -> 24 bytes with a 4-byte pointer. */
    uint8_t bitmap[24];
    memset(bitmap, 0, sizeof(bitmap));
    put32(bitmap + 4, g->width);
    put32(bitmap + 8, g->height);
    put32(bitmap + 12, g->width * 4u);
    bitmap[16] = 1;   /* bmPlanes */
    bitmap[18] = 32;  /* bmBitsPixel */
    int rc = gm_write(sg->memory, args[2], bitmap, sizeof(bitmap));
    if (rc) return rc;
    *result = 24;
    return GM_OK;
}
int gsg_bit_blt(GuestShellGdi *sg, const uint32_t *args, uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    uint32_t dest = args[0], src = args[5];
    uint32_t rop = args[8];
    if (dest == GSG_SCREEN_DC || src == GSG_SCREEN_DC) {
        /* No window surface is composed here: a screen blit is a logged no-op
         * so the guest can continue with the documented TRUE. */
        sg->screen_bitblt_calls++;
        note(sg, "BitBlt to/from the screen DC: no-op (no composed surface)");
        *result = 1;
        return GM_OK;
    }
    GsgObject *dest_dc = gsg_lookup(dest);
    GsgObject *src_dc = gsg_lookup(src);
    if (!dest_dc || dest_dc->kind != 1 || !src_dc || src_dc->kind != 1) {
        int rc = last_error(sg, GSG_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    if (rop != 0x00cc0020u)
        return stop(sg, GU_STOP_GSG_ROP,
                    "BitBlt: only SRCCOPY is modelled");
    if (!dest_dc->selected || !src_dc->selected)
        return stop(sg, GU_STOP_GSG_NO_BITMAP,
                    "BitBlt: a memory DC has no selected bitmap");
    GsgObject *dest_bitmap = gsg_lookup(dest_dc->selected);
    GsgObject *src_bitmap = gsg_lookup(src_dc->selected);
    if (!dest_bitmap || !src_bitmap)
        return stop(sg, GU_STOP_GSG_BAD_HANDLE,
                    "BitBlt: selected bitmap is not live");
    int32_t dx = (int32_t)args[1], dy = (int32_t)args[2];
    int32_t w = (int32_t)args[3], h = (int32_t)args[4];
    int32_t sx = (int32_t)args[6], sy = (int32_t)args[7];
    if (w <= 0 || h <= 0) { *result = 1; return GM_OK; }
    for (int32_t row = 0; row < h; row++) {
        int32_t ty = dy + row, fy = sy + row;
        if (ty < 0 || fy < 0) continue;
        if ((uint32_t)ty >= dest_bitmap->height ||
            (uint32_t)fy >= src_bitmap->height) continue;
        for (int32_t column = 0; column < w; column++) {
            int32_t tx = dx + column, fx = sx + column;
            if (tx < 0 || fx < 0) continue;
            if ((uint32_t)tx >= dest_bitmap->width ||
                (uint32_t)fx >= src_bitmap->width) continue;
            dest_bitmap->bits[(size_t)ty * dest_bitmap->width + (uint32_t)tx] =
                src_bitmap->bits[(size_t)fy * src_bitmap->width + (uint32_t)fx];
        }
    }
    *result = 1;
    return GM_OK;
}

/* --- imm32 / powrprof / dbghelp ---------------------------------------- */
int gsg_imm_disable_ime(GuestShellGdi *sg, const uint32_t *args,
                        uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    /* No IME is attached to this process, so the documented TRUE holds. */
    *result = 1;
    return GM_OK;
}
int gsg_get_pwr_capabilities(GuestShellGdi *sg, const uint32_t *args,
                             uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    if (!args[0]) {
        int rc = last_error(sg, GSG_ERROR_INVALID_PARAMETER);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    /* SYSTEM_POWER_CAPABILITIES is 76 bytes on PE32. Zeroed except the two
     * "supported" capability bits that hold on every modern machine; no
     * battery, no UPS, no lid, no sleep states are claimed. */
    uint8_t caps[76];
    memset(caps, 0, sizeof(caps));
    caps[13] = 1; /* ThermalControl */
    caps[14] = 1; /* ProcessorThrottle */
    int rc = gm_write(sg->memory, args[0], caps, sizeof(caps));
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}
static int dbghelp_refusal(GuestShellGdi *sg, const char *name,
                           uint32_t *result) {
    sg->dbghelp_calls++;
    fprintf(stderr, "dbghelp: %s -> FALSE (no symbol handler, no symbol "
            "server in this runtime)\n", name);
    note(sg, name);
    int rc = last_error(sg, GSG_ERROR_INVALID_PARAMETER);
    if (rc) return rc;
    *result = 0;
    return GM_OK;
}
int gsg_sym_initialize(GuestShellGdi *sg, const uint32_t *args,
                       uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    return dbghelp_refusal(sg, "SymInitialize", result);
}
int gsg_sym_set_options(GuestShellGdi *sg, const uint32_t *args,
                        uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    /* SymSetOptions returns the previous mask; nothing is enabled here. */
    sg->dbghelp_calls++;
    *result = 0;
    return GM_OK;
}
int gsg_sym_function_table_access64(GuestShellGdi *sg, const uint32_t *args,
                                    uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    return dbghelp_refusal(sg, "SymFunctionTableAccess64", result);
}
int gsg_sym_get_module_base64(GuestShellGdi *sg, const uint32_t *args,
                              uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    return dbghelp_refusal(sg, "SymGetModuleBase64", result);
}
int gsg_stack_walk64(GuestShellGdi *sg, const uint32_t *args,
                     uint32_t *result) {
    if (!sg || !args || !result) return GU_STOP_GSG_BAD_ARGS;
    return dbghelp_refusal(sg, "StackWalk64", result);
}

/* --- registration lists ------------------------------------------------- */
#define GSG_ARGS(n) ((n) * 4u)
const GuestShellGdiExport shell32_exports[] = {
    { "shell32.dll", "SHGetFolderPathW", GSG_ARGS(5),
      gsg_sh_get_folder_path_w },
    { "shell32.dll", "SHGetSpecialFolderPathA", GSG_ARGS(4),
      gsg_sh_get_special_folder_path_a },
    { "shell32.dll", "SHGetSpecialFolderPathW", GSG_ARGS(4),
      gsg_sh_get_special_folder_path_w },
    { "shell32.dll", "SHCreateDirectoryExA", GSG_ARGS(3),
      gsg_sh_create_directory_ex_a },
    { "shell32.dll", "ShellExecuteA", GSG_ARGS(6), gsg_shell_execute_a },
    { "shell32.dll", "ShellExecuteExA", GSG_ARGS(1), gsg_shell_execute_ex_a },
    /* Implemented in runtime/guest_commandline.c (gc_argv); fn stays NULL so
     * the wiring layer resolves it there instead of here. */
    { "shell32.dll", "CommandLineToArgvW", GSG_ARGS(2), NULL }
};
const uint32_t shell32_export_count =
    (uint32_t)(sizeof(shell32_exports) / sizeof(shell32_exports[0]));

const GuestShellGdiExport gdi32_exports[] = {
    { "gdi32.dll", "GetStockObject", GSG_ARGS(1), gsg_get_stock_object },
    { "gdi32.dll", "CreateCompatibleDC", GSG_ARGS(1), gsg_create_compatible_dc },
    { "gdi32.dll", "CreateCompatibleBitmap", GSG_ARGS(3),
      gsg_create_compatible_bitmap },
    { "gdi32.dll", "SelectObject", GSG_ARGS(2), gsg_select_object },
    { "gdi32.dll", "DeleteObject", GSG_ARGS(1), gsg_delete_object },
    { "gdi32.dll", "DeleteDC", GSG_ARGS(1), gsg_delete_dc },
    { "gdi32.dll", "GetDeviceCaps", GSG_ARGS(2), gsg_get_device_caps },
    { "gdi32.dll", "GetObjectA", GSG_ARGS(3), gsg_get_object_a },
    { "gdi32.dll", "BitBlt", GSG_ARGS(9), gsg_bit_blt }
};
const uint32_t gdi32_export_count =
    (uint32_t)(sizeof(gdi32_exports) / sizeof(gdi32_exports[0]));

const GuestShellGdiExport misc_extra_exports[] = {
    { "imm32.dll", "ImmDisableIME", GSG_ARGS(1), gsg_imm_disable_ime },
    { "powrprof.dll", "GetPwrCapabilities", GSG_ARGS(1),
      gsg_get_pwr_capabilities },
    { "dbghelp.dll", "SymInitialize", GSG_ARGS(3), gsg_sym_initialize },
    { "dbghelp.dll", "SymSetOptions", GSG_ARGS(1), gsg_sym_set_options },
    { "dbghelp.dll", "SymFunctionTableAccess64", GSG_ARGS(2),
      gsg_sym_function_table_access64 },
    { "dbghelp.dll", "SymGetModuleBase64", GSG_ARGS(2),
      gsg_sym_get_module_base64 },
    { "dbghelp.dll", "StackWalk64", GSG_ARGS(9), gsg_stack_walk64 }
};
const uint32_t misc_extra_export_count =
    (uint32_t)(sizeof(misc_extra_exports) / sizeof(misc_extra_exports[0]));
