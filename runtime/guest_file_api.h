#ifndef BURNOUT_GUEST_FILE_API_H
#define BURNOUT_GUEST_FILE_API_H
#include "guest_memory.h"
#include "guest_locale.h"
#include "guest_path_map.h"

/* kernel32 file/find/path bridges over the guest path projection.
 *
 * Guest paths are resolved through guest_path_map.h (C:\BurnoutPR -> game data
 * root, C:\Users\<user>\... -> writable user root). ANSI names are CP1252 and
 * are converted with the installed GuestLocale (no host locale, no UTF-8
 * assumption). Wide names are BMP UTF-16; surrogates stop explicitly.
 *
 * Win32 return values are written through *result. LastError is written to the
 * guest TEB (+0x34) only on failure, matching Win32 (success does not clear it).
 * A non-zero function return is a named model stop (GM_*), never a Win32 status.
 *
 * Documented limits:
 *   - one guest process: FILE_SHARE_* is accepted and ignored (no second
 *     opener can observe it), lpSecurityAttributes and hTemplateFile must be 0
 *   - FILE_FLAG_OVERLAPPED, lpOverlapped != 0, FILE_FLAG_NO_BUFFERING,
 *     FILE_FLAG_DELETE_ON_CLOSE and asynchronous/IO-completion semantics are
 *     named stops
 *   - directories can be created, deleted and stat'ed, but not opened as files
 *     (CreateFile on a directory -> ERROR_ACCESS_DENIED)
 *   - FindFirstFile skips "." and "..", cAlternateFileName is empty and only
 *     "*" / "*.*" (all) and "?" (one character) are wildcards; "." is literal
 *   - GetFileAttributesEx only accepts GetFileExInfoStandard (0); any other
 *     level is the documented ERROR_INVALID_PARAMETER
 *   - DeleteFile uses POSIX unlink semantics: deleting a file that still has an
 *     open handle succeeds (no Windows sharing violation is modelled)
 *   - the mapping roots are runtime-owned directories; symlinks inside them are
 *     followed by the host, ".." can never leave a root
 *   - guest path strings are bounded to 1024 bytes / units
 *   - no file locking, no reparse points, no alternate data streams, no ACLs */

enum {
    GFA_ERROR_FILE_NOT_FOUND=2,
    GFA_ERROR_PATH_NOT_FOUND=3,
    GFA_ERROR_ACCESS_DENIED=5,
    GFA_ERROR_INVALID_HANDLE=6,
    GFA_ERROR_NOT_ENOUGH_MEMORY=8,
    GFA_ERROR_NO_MORE_FILES=18,
    GFA_ERROR_WRITE_FAULT=29,
    GFA_ERROR_READ_FAULT=30,
    GFA_ERROR_HANDLE_EOF=38,
    GFA_ERROR_FILE_EXISTS=80,
    GFA_ERROR_INVALID_PARAMETER=87,
    GFA_ERROR_DISK_FULL=112,
    GFA_ERROR_INVALID_NAME=123,
    GFA_ERROR_NEGATIVE_SEEK=131,
    GFA_ERROR_ALREADY_EXISTS=183,
    GFA_ERROR_FILENAME_EXCED_RANGE=206
};

#define GFA_INVALID_HANDLE_VALUE 0xffffffffu
#define GFA_INVALID_FILE_ATTRIBUTES 0xffffffffu
#define GFA_INVALID_SET_FILE_POINTER 0xffffffffu
#define GFA_FILE_TYPE_DISK 1u
#define GFA_FILE_TYPE_UNKNOWN 0u
#define GFA_FIND_DATA_A_SIZE 318u
#define GFA_FIND_DATA_W_SIZE 592u
#define GFA_FILE_ATTRIBUTE_DATA_SIZE 36u
#define GFA_DEFAULT_CURRENT_DIRECTORY "C:\\BurnoutPR"

typedef struct GuestFileApi GuestFileApi;

/* The locale is borrowed and must stay alive; when it is NULL or has no
 * installed code page the ANSI entry points stop instead of guessing bytes. */
int gfa_create(GuestPathMap *,const GuestLocale *,GuestFileApi **out);
void gfa_destroy(GuestFileApi *);
const char *gfa_current_directory(const GuestFileApi *);

int gfa_create_file_a(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                      uint32_t access,uint32_t share,uint32_t security,
                      uint32_t disposition,uint32_t flags,uint32_t templ,
                      uint32_t *result);
int gfa_create_file_w(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                      uint32_t access,uint32_t share,uint32_t security,
                      uint32_t disposition,uint32_t flags,uint32_t templ,
                      uint32_t *result);
int gfa_read_file(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t buffer,
                  uint32_t to_read,uint32_t read_out,uint32_t overlapped,
                  uint32_t *result);
int gfa_write_file(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t buffer,
                   uint32_t to_write,uint32_t written_out,uint32_t overlapped,
                   uint32_t *result);
int gfa_set_file_pointer(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t low,
                         uint32_t high,uint32_t method,uint32_t *result);
int gfa_set_file_pointer_ex(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t low,
                            uint32_t high,uint32_t new_pointer,uint32_t method,
                            uint32_t *result);
int gfa_get_file_size(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t high_out,
                      uint32_t *result);
int gfa_get_file_size_ex(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t out,
                         uint32_t *result);
int gfa_get_file_attributes_a(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                              uint32_t *result);
int gfa_get_file_attributes_w(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                              uint32_t *result);
int gfa_get_file_attributes_ex_a(GuestMemory *,uint32_t teb,GuestFileApi *,
                                 uint32_t name,uint32_t level,uint32_t data,
                                 uint32_t *result);
int gfa_get_file_attributes_ex_w(GuestMemory *,uint32_t teb,GuestFileApi *,
                                 uint32_t name,uint32_t level,uint32_t data,
                                 uint32_t *result);
int gfa_find_first_file_a(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t pattern,
                          uint32_t data,uint32_t *result);
int gfa_find_first_file_w(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t pattern,
                          uint32_t data,uint32_t *result);
int gfa_find_next_file_a(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t handle,
                         uint32_t data,uint32_t *result);
int gfa_find_next_file_w(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t handle,
                         uint32_t data,uint32_t *result);
int gfa_find_close(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);
int gfa_create_directory_a(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                           uint32_t security,uint32_t *result);
int gfa_create_directory_w(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                           uint32_t security,uint32_t *result);
int gfa_delete_file_a(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                      uint32_t *result);
int gfa_delete_file_w(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                      uint32_t *result);
int gfa_get_file_type(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);
int gfa_flush_file_buffers(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);
int gfa_get_current_directory_a(GuestMemory *,uint32_t teb,GuestFileApi *,
                                uint32_t capacity,uint32_t buffer,uint32_t *result);
int gfa_get_current_directory_w(GuestMemory *,uint32_t teb,GuestFileApi *,
                                uint32_t capacity,uint32_t buffer,uint32_t *result);
int gfa_set_current_directory_a(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                                uint32_t *result);
int gfa_set_current_directory_w(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                                uint32_t *result);
int gfa_get_full_path_name_a(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                             uint32_t capacity,uint32_t buffer,uint32_t file_part,
                             uint32_t *result);
int gfa_get_full_path_name_w(GuestMemory *,uint32_t teb,GuestFileApi *,uint32_t name,
                             uint32_t capacity,uint32_t buffer,uint32_t file_part,
                             uint32_t *result);
/* CloseHandle for GH_KIND_FILE / GH_KIND_FIND; other kinds are left to their
 * owner (named stop) so the dispatcher can try the next module. */
int gfa_close_handle(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);

/* Registration list for runtime/win32_api_table.c (wired later; that file is
 * not modified here). stdcall_arg_bytes is the number of stack bytes the callee
 * pops, i.e. the sum of the Win32 parameter sizes. */
typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    /* Unprototyped on purpose: the entries have different C signatures and are
     * dispatched by the wiring layer, never called through this field here. */
    int (*fn)();
} GuestFileExport;

extern const GuestFileExport kernel32_file_exports[];
extern const uint32_t kernel32_file_export_count;
#endif
