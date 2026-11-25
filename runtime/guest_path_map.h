#ifndef BURNOUT_GUEST_PATH_MAP_H
#define BURNOUT_GUEST_PATH_MAP_H
#include <stdint.h>

/* Guest Windows path -> host path projection for the Android port.
 *
 * Two mapping tables:
 *   primary : C:\BurnoutPR\...            -> <game_root>   (port data dir)
 *   user    : C:\Users\<user>\Documents\...,
 *             C:\Users\<user>\AppData\Roaming|Local|LocalLow\...,
 *             C:\ProgramData\...          -> <user_root>/... (saves/config)
 *
 * Guest paths handled here are UTF-8; the file API converts CP1252 (ANSI) and
 * UTF-16 (wide) first. Canonical guest form: upper-case drive letter, backslash
 * separators, no "." components, ".." resolved and never above the drive root,
 * no trailing separator. Resolution on a case-sensitive host filesystem is
 * case-insensitive per component by scanning the directory, with a bounded
 * cache of (parent, folded name) -> actual name. A guest path that no mapping
 * covers is reported as GPM_UNMAPPED; the caller turns that into the ordinary
 * "this file does not exist in the port" Win32 answer instead of inventing a
 * host location.
 *
 * No host pointer is ever handed to the guest; the host path stays in the
 * runtime object. Symlinks inside the roots are not followed out of the root by
 * ".." normalisation (there is no ".." left after normalisation); the roots
 * themselves are runtime-owned directories. */

enum { GPM_MODE_READ=0, GPM_MODE_WRITE=1, GPM_MODE_DIR=2 };
enum { GPM_OK=0, GPM_NOT_FOUND=1, GPM_PATH_NOT_FOUND=2, GPM_UNMAPPED=3,
       GPM_INVALID=4, GPM_TOO_LONG=5, GPM_ACCESS=6, GPM_IO=7 };

#define GPM_MAX_PATH 1024u
#define GPM_MAX_HOST 2048u

typedef struct GuestPathMap GuestPathMap;

/* Creates the roots (mkdir -p) so saves/config directories exist. Both roots
 * must be absolute host paths. */
int gpm_create(const char *game_root,const char *user_root,GuestPathMap **out);
void gpm_destroy(GuestPathMap *);
/* Canonical guest form; GPM_INVALID for UNC/device/relative/escaping paths. */
int gpm_normalize(const char *guest_utf8,char *out,uint32_t capacity);
/* Canonical guest path -> host path without touching the filesystem. */
int gpm_map(GuestPathMap *,const char *guest_utf8,char *out,uint32_t capacity);
/* Canonical guest path -> host path, case-insensitively resolved. READ/DIR
 * require the final component to exist (DIR also requires a directory);
 * WRITE allows an absent final component but requires the parent. */
int gpm_resolve(GuestPathMap *,const char *guest_utf8,uint32_t mode,
                char *out,uint32_t capacity);
/* Optional read overlay: when set, READ resolution of C:\\BurnoutPR\\<rest>
 * first tries <mod_root>/<rest> (case-insensitive) and uses it if it exists.
 * Writes and directory lookups always use the game root. NULL/"" clears it. */
int gpm_set_mod_root(GuestPathMap *,const char *mod_root);
#endif
