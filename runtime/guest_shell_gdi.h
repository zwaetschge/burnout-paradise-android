#ifndef BURNOUT_GUEST_SHELL_GDI_H
#define BURNOUT_GUEST_SHELL_GDI_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_bridge_abi.h"
#include "guest_locale.h"
#include "guest_path_map.h"
#include "guest_file_api.h"

/* shell32 / gdi32 / imm32 / powrprof / dbghelp bridges.
 *
 * shell32
 *   - SHGetFolderPathW, SHGetSpecialFolderPathA/W: the CSIDL table is projected
 *     onto the *same* guest paths that runtime/guest_path_map.c maps, so a save
 *     written through C:\Users\Player\Documents lands in the port user root.
 *     An unmapped CSIDL is the documented E_INVALIDARG (W) / FALSE (A/W), never
 *     an invented directory. CSIDL_FLAG_CREATE creates the mapped directory.
 *   - SHCreateDirectoryExA: recursive creation through the path projection
 *     (gpm_resolve per component); the documented 0 / ERROR_ALREADY_EXISTS /
 *     ERROR_FILE_EXISTS / ERROR_PATH_NOT_FOUND / ERROR_BAD_PATHNAME results.
 *   - ShellExecuteA/ExA: this runtime cannot start a child process, so the
 *     documented failure is returned (SE_ERR_ACCESSDENIED = 5, or FALSE with
 *     ERROR_ACCESS_DENIED) and the request is logged. Nothing is launched.
 *   - CommandLineToArgvW is implemented in runtime/guest_commandline.c; only the
 *     registration entry is repeated here (fn = NULL marks it as external).
 *
 * gdi32
 *   - Real memory DCs and bitmaps: a bitmap owns a host-side 32-bpp buffer, a
 *     memory DC owns a selected bitmap, and BitBlt between two memory DCs copies
 *     the pixels for real (SRCCOPY only). A screen DC cannot be obtained through
 *     the imported entry points, so a BitBlt that names one is a logged no-op
 *     (documented: no window surface is composed here).
 *   - GetDeviceCaps answers from the host surface size (HORZRES/VERTRES),
 *     BITSPIXEL = 32 and LOGPIXELS = 96; other indices answer 0 and are counted.
 *   - GetObjectA returns the 18-byte PE32 BITMAP for a bitmap and 0 otherwise.
 *
 * imm32 ImmDisableIME returns TRUE (no IME is attached to this process).
 * powrprof GetPwrCapabilities fills a zeroed SYSTEM_POWER_CAPABILITIES with the
 *   conservative "desktop, no battery, no UPS" flags.
 * dbghelp: no symbol handler and no symbol server exist, so SymInitialize,
 *   SymSetOptions, SymFunctionTableAccess64, SymGetModuleBase64 and StackWalk64
 *   answer the documented failure (FALSE / 0) after logging.
 *
 * Guest memory is only touched through gm_check/gm_read/gm_write. Kernel-object
 * handles come from runtime/guest_handles.h (GH_KIND_OTHER); no host pointer is
 * ever written into guest memory. A non-zero function return is a named stop.
 */

#define GSG_MAX_PATH 260u
#define GSG_MAX_GUEST_PATH 1024u
#define GSG_SCREEN_DC 0x00010001u
#define GSG_STOCK_BASE 0x00a00000u
#define GSG_STOCK_COUNT 20u
#define GSG_MAX_BITMAP_EDGE 8192u

#define GSG_ERROR_SUCCESS 0u
#define GSG_ERROR_FILE_NOT_FOUND 2u
#define GSG_ERROR_PATH_NOT_FOUND 3u
#define GSG_ERROR_ACCESS_DENIED 5u
#define GSG_ERROR_INVALID_HANDLE 6u
#define GSG_ERROR_NOT_ENOUGH_MEMORY 8u
#define GSG_ERROR_INVALID_PARAMETER 87u
#define GSG_ERROR_INVALID_NAME 123u
#define GSG_ERROR_BAD_PATHNAME 161u
#define GSG_ERROR_ALREADY_EXISTS 183u
#define GSG_ERROR_FILE_EXISTS 80u
#define GSG_SE_ERR_ACCESSDENIED 5u

#define GSG_S_OK 0x00000000u
#define GSG_E_INVALIDARG 0x80070057u
#define GSG_SHGFP_TYPE_CURRENT 0u
#define GSG_SHGFP_TYPE_DEFAULT 1u
#define GSG_CSIDL_FLAG_CREATE 0x8000u

/* CSIDL values used by the projection. */
enum {
    GSG_CSIDL_DESKTOP = 0x00,
    GSG_CSIDL_INTERNET = 0x01,
    GSG_CSIDL_PROGRAMS = 0x02,
    GSG_CSIDL_PERSONAL = 0x05,
    GSG_CSIDL_FAVORITES = 0x06,
    GSG_CSIDL_STARTUP = 0x07,
    GSG_CSIDL_SENDTO = 0x09,
    GSG_CSIDL_STARTMENU = 0x0b,
    GSG_CSIDL_MYMUSIC = 0x0d,
    GSG_CSIDL_MYVIDEO = 0x0e,
    GSG_CSIDL_DESKTOPDIRECTORY = 0x10,
    GSG_CSIDL_TEMPLATES = 0x15,
    GSG_CSIDL_COMMON_STARTMENU = 0x16,
    GSG_CSIDL_COMMON_PROGRAMS = 0x17,
    GSG_CSIDL_COMMON_STARTUP = 0x18,
    GSG_CSIDL_FONTS = 0x14,
    GSG_CSIDL_APPDATA = 0x1a,
    GSG_CSIDL_LOCAL_APPDATA = 0x1c,
    GSG_CSIDL_INTERNET_CACHE = 0x20,
    GSG_CSIDL_COOKIES = 0x21,
    GSG_CSIDL_HISTORY = 0x22,
    GSG_CSIDL_COMMON_APPDATA = 0x23,
    GSG_CSIDL_WINDOWS = 0x24,
    GSG_CSIDL_SYSTEM = 0x25,
    GSG_CSIDL_PROGRAM_FILES = 0x26,
    GSG_CSIDL_MYPICTURES = 0x27,
    GSG_CSIDL_PROFILE = 0x28,
    GSG_CSIDL_PROGRAM_FILESX86 = 0x2a,
    GSG_CSIDL_COMMON_DOCUMENTS = 0x2e,
    GSG_CSIDL_CDBURN_AREA = 0x3b
};

/* gdi32 stock object indices. */
enum {
    GSG_WHITE_BRUSH = 0, GSG_LTGRAY_BRUSH = 1, GSG_GRAY_BRUSH = 2,
    GSG_DKGRAY_BRUSH = 3, GSG_BLACK_BRUSH = 4, GSG_NULL_BRUSH = 5,
    GSG_WHITE_PEN = 6, GSG_BLACK_PEN = 7, GSG_NULL_PEN = 8,
    GSG_OEM_FIXED_FONT = 10, GSG_ANSI_FIXED_FONT = 11,
    GSG_ANSI_VAR_FONT = 12, GSG_SYSTEM_FONT = 13,
    GSG_DEVICE_DEFAULT_FONT = 14, GSG_DEFAULT_PALETTE = 15,
    GSG_SYSTEM_FIXED_FONT = 16, GSG_DEFAULT_GUI_FONT = 17,
    GSG_DC_BRUSH = 18, GSG_DC_PEN = 19
};

/* Module-local named stops. */
enum {
    GU_STOP_GSG_NO_LOCALE = -2301,   /* ANSI entry point without CP1252 */
    GU_STOP_GSG_NON_BMP = -2302,     /* surrogate in a guest string */
    GU_STOP_GSG_PATH_TOO_LONG = -2303,
    GU_STOP_GSG_BAD_HANDLE = -2304,  /* not a DC/bitmap issued here */
    GU_STOP_GSG_ROP = -2305,         /* BitBlt raster op not modelled */
    GU_STOP_GSG_NO_BITMAP = -2306,   /* memory DC without a bitmap */
    GU_STOP_GSG_BAD_ARGS = -2307,
    GU_STOP_GSG_SECURITY_ATTR = -2308
};

typedef struct GuestShellGdi GuestShellGdi;

/* The path map and locale are borrowed and must stay alive. The file API is
 * optional (only SHCreateDirectoryExA needs the projection, not the API). */
int gsg_create(GuestMemory *, uint32_t teb, GuestPathMap *, const GuestLocale *,
               GuestShellGdi **out);
void gsg_destroy(GuestShellGdi *);
int gsg_set_surface_size(GuestShellGdi *, uint32_t width, uint32_t height);
uint32_t gsg_surface_width(const GuestShellGdi *);
uint32_t gsg_surface_height(const GuestShellGdi *);

/* Diagnostics. */
uint32_t gsg_screen_bitblt_calls(const GuestShellGdi *);
uint32_t gsg_unknown_device_caps(const GuestShellGdi *);
uint32_t gsg_shell_execute_calls(const GuestShellGdi *);
uint32_t gsg_dbghelp_calls(const GuestShellGdi *);
const char *gsg_last_log(const GuestShellGdi *);
const char *gsg_stop_name(int code);

/* --- shell32 ----------------------------------------------------------- */
int gsg_sh_get_folder_path_w(GuestShellGdi *, const uint32_t *args,
                             uint32_t *result);
int gsg_sh_get_special_folder_path_a(GuestShellGdi *, const uint32_t *args,
                                     uint32_t *result);
int gsg_sh_get_special_folder_path_w(GuestShellGdi *, const uint32_t *args,
                                     uint32_t *result);
int gsg_sh_create_directory_ex_a(GuestShellGdi *, const uint32_t *args,
                                 uint32_t *result);
int gsg_shell_execute_a(GuestShellGdi *, const uint32_t *args,
                        uint32_t *result);
int gsg_shell_execute_ex_a(GuestShellGdi *, const uint32_t *args,
                           uint32_t *result);

/* --- gdi32 ------------------------------------------------------------- */
int gsg_get_stock_object(GuestShellGdi *, const uint32_t *args,
                         uint32_t *result);
int gsg_create_compatible_dc(GuestShellGdi *, const uint32_t *args,
                             uint32_t *result);
int gsg_create_compatible_bitmap(GuestShellGdi *, const uint32_t *args,
                                 uint32_t *result);
int gsg_select_object(GuestShellGdi *, const uint32_t *args, uint32_t *result);
int gsg_delete_object(GuestShellGdi *, const uint32_t *args, uint32_t *result);
int gsg_delete_dc(GuestShellGdi *, const uint32_t *args, uint32_t *result);
int gsg_get_device_caps(GuestShellGdi *, const uint32_t *args,
                        uint32_t *result);
int gsg_get_object_a(GuestShellGdi *, const uint32_t *args, uint32_t *result);
int gsg_bit_blt(GuestShellGdi *, const uint32_t *args, uint32_t *result);

/* --- imm32 / powrprof / dbghelp ---------------------------------------- */
int gsg_imm_disable_ime(GuestShellGdi *, const uint32_t *args,
                        uint32_t *result);
int gsg_get_pwr_capabilities(GuestShellGdi *, const uint32_t *args,
                             uint32_t *result);
int gsg_sym_initialize(GuestShellGdi *, const uint32_t *args,
                       uint32_t *result);
int gsg_sym_set_options(GuestShellGdi *, const uint32_t *args,
                        uint32_t *result);
int gsg_sym_function_table_access64(GuestShellGdi *, const uint32_t *args,
                                    uint32_t *result);
int gsg_sym_get_module_base64(GuestShellGdi *, const uint32_t *args,
                              uint32_t *result);
int gsg_stack_walk64(GuestShellGdi *, const uint32_t *args, uint32_t *result);

/* Registration lists for runtime/win32_api_table.c (wired later). */
typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestShellGdi *, const uint32_t *args, uint32_t *result);
} GuestShellGdiExport;

extern const GuestShellGdiExport shell32_exports[];
extern const uint32_t shell32_export_count;
extern const GuestShellGdiExport gdi32_exports[];
extern const uint32_t gdi32_export_count;
extern const GuestShellGdiExport misc_extra_exports[];
extern const uint32_t misc_extra_export_count;

#endif
