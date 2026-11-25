/* Win32 bridges the original (2009, ANSI) Burnout Paradise needs beyond the set
 * the Remastered build exercised: profile (INI) files, the ANSI locale queries
 * of the VS2005-era CRT, drive/disk queries and a few console/loader calls.
 *
 * Same contract as runtime/static_bridges.c: stdcall arguments arrive in a[],
 * the result goes to *r, a call this runtime cannot answer truthfully returns
 * GM_UNSUPPORTED (a named stop) instead of a guessed value.  Guest strings are
 * read through the flat mapping (port_flat_base()).
 */
#include "recomp_types.h"
#include "static_bridges.h"
#include "guest_path_map.h"
#include "orig_bridges.h"
#include "guest_user32.h"
#include "d3d9_thunk.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>

uint8_t *port_flat_base(void);
int static_load_library(const char *name, uint32_t *handle);

#define GA(i) (a[(i)])
static char *gstr(uint32_t va) { return va ? (char *)port_flat_base() + va : NULL; }
static void set_last_error(uint32_t e) { gm_write(g_mem, g_teb + 0x34, &e, 4); }
static int dbg(const char *what) { const char *e = getenv("PORT_DBG_ORIG"); return e && (e[0] == '1' || strstr(e, what)); }

/* ================================================================== */
/* profile (INI) files                                                 */
/* ================================================================== */

typedef struct { char **line; size_t count, cap; } Ini;

static void ini_free(Ini *f) { for (size_t i = 0; i < f->count; i++) free(f->line[i]); free(f->line); f->line = NULL; f->count = f->cap = 0; }
static int ini_push(Ini *f, const char *s, size_t n, size_t at) {
    if (f->count == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 64;
        char **nl = realloc(f->line, cap * sizeof *nl);
        if (!nl) return -1;
        f->line = nl; f->cap = cap;
    }
    char *c = malloc(n + 1);
    if (!c) return -1;
    memcpy(c, s, n); c[n] = 0;
    if (at > f->count) at = f->count;
    memmove(&f->line[at + 1], &f->line[at], (f->count - at) * sizeof *f->line);
    f->line[at] = c; f->count++;
    return 0;
}
static void ini_remove(Ini *f, size_t at) {
    free(f->line[at]);
    memmove(&f->line[at], &f->line[at + 1], (f->count - at - 1) * sizeof *f->line);
    f->count--;
}
/* Guest file name -> host path. A name without a drive would live in the
 * Windows directory, which this runtime does not project: reads see "no such
 * file" (so defaults apply), writes fail. */
static int ini_host(uint32_t name_va, uint32_t mode, char *host, uint32_t cap) {
    const char *name = gstr(name_va);
    if (!name || !name[0]) return -1;
    if (!(isalpha((unsigned char)name[0]) && name[1] == ':')) return -1;
    return gpm_resolve(g_pmap, name, mode, host, cap) == GPM_OK ? 0 : -1;
}
static int ini_load(const char *host, Ini *f) {
    memset(f, 0, sizeof *f);
    FILE *fp = fopen(host, "rb");
    if (!fp) return 0;                              /* missing file = empty profile */
    char *buf = NULL; size_t n = 0, cap = 0; int c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') { size_t l = n; if (l && buf[l - 1] == '\r') l--; if (ini_push(f, buf ? buf : "", l, f->count)) { fclose(fp); free(buf); return -1; } n = 0; continue; }
        if (n + 1 > cap) { cap = cap ? cap * 2 : 256; char *nb = realloc(buf, cap); if (!nb) { fclose(fp); free(buf); return -1; } buf = nb; }
        buf[n++] = (char)c;
    }
    if (n) { size_t l = n; if (buf[l - 1] == '\r') l--; if (ini_push(f, buf, l, f->count)) { fclose(fp); free(buf); return -1; } }
    fclose(fp); free(buf);
    return 0;
}
static int ini_save(const char *host, const Ini *f) {
    FILE *fp = fopen(host, "wb");
    if (!fp) return -1;
    for (size_t i = 0; i < f->count; i++) { fputs(f->line[i], fp); fputs("\r\n", fp); }
    return fclose(fp) ? -1 : 0;
}
static const char *skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
/* "[name]" header? returns 1 and the name span. */
static int ini_header(const char *line, const char **name, size_t *len) {
    const char *s = skip_ws(line);
    if (*s != '[') return 0;
    const char *e = strchr(s, ']');
    if (!e) return 0;
    *name = s + 1; *len = (size_t)(e - s - 1);
    return 1;
}
static int span_ieq(const char *s, size_t n, const char *t) { return strlen(t) == n && !strncasecmp(s, t, n); }
/* Section [first, end) line range (first = line after the header). */
static int ini_section(const Ini *f, const char *app, size_t *first, size_t *end) {
    for (size_t i = 0; i < f->count; i++) {
        const char *nm; size_t nl;
        if (!ini_header(f->line[i], &nm, &nl) || !span_ieq(nm, nl, app)) continue;
        size_t j = i + 1;
        while (j < f->count) { const char *n2; size_t l2; if (ini_header(f->line[j], &n2, &l2)) break; j++; }
        *first = i + 1; *end = j;
        return 1;
    }
    return 0;
}
/* "key = value" line? returns 1 with the trimmed key span and value start. */
static int ini_keyline(const char *line, const char **key, size_t *klen, const char **value) {
    const char *s = skip_ws(line);
    if (!*s || *s == ';' || *s == '[') return 0;
    const char *eq = strchr(s, '=');
    if (!eq) return 0;
    const char *ke = eq;
    while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    *key = s; *klen = (size_t)(ke - s); *value = eq + 1;
    return 1;
}
/* Value as the API returns it: blanks trimmed, one pair of matching quotes removed. */
static size_t ini_value(const char *v, char *out, size_t cap) {
    v = skip_ws(v);
    size_t n = strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
    if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) { v++; n -= 2; }
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n); out[n] = 0;
    return n;
}
static int ini_find(const Ini *f, const char *app, const char *key, char *out, size_t cap) {
    size_t a0, a1;
    if (!ini_section(f, app, &a0, &a1)) return 0;
    for (size_t i = a0; i < a1; i++) {
        const char *k, *v; size_t kl;
        if (ini_keyline(f->line[i], &k, &kl, &v) && span_ieq(k, kl, key)) { ini_value(v, out, cap); return 1; }
    }
    return 0;
}
/* Copy a single string result; returns the API's count. */
static uint32_t put_string(uint32_t out_va, uint32_t size, const char *s) {
    if (!out_va || !size) return 0;
    size_t n = strlen(s);
    if (n > size - 1) n = size - 1;
    memcpy(gstr(out_va), s, n); gstr(out_va)[n] = 0;
    return (uint32_t)n;
}
/* Multi-string list builder ("a\0b\0\0"), truncating the way the API does (size-2). */
typedef struct { char *out; uint32_t size, used; int truncated; } Multi;
static void multi_add(Multi *m, const char *s, size_t n) {
    if (m->truncated || m->size < 2) { m->truncated = 1; return; }
    if (m->used + n + 1 > m->size - 1) {
        size_t room = (m->size - 2 > m->used) ? m->size - 2 - m->used : 0;
        memcpy(m->out + m->used, s, room); m->used += (uint32_t)room;
        m->truncated = 1;
        return;
    }
    memcpy(m->out + m->used, s, n); m->used += (uint32_t)n; m->out[m->used++] = 0;
}
static uint32_t multi_end(Multi *m) {
    if (!m->size) return 0;
    if (m->truncated) { if (m->size >= 2) { m->out[m->size - 2] = 0; m->out[m->size - 1] = 0; return m->size - 2; } m->out[0] = 0; return 0; }
    m->out[m->used] = 0;
    return m->used;                             /* every string's NUL counts, the final terminator does not */
}

static int ob_get_private_profile_string_a(const uint32_t *a, uint32_t *r) {
    const char *app = gstr(GA(0)), *key = gstr(GA(1)), *def = gstr(GA(2));
    uint32_t out = GA(3), size = GA(4);
    char host[GPM_MAX_HOST];
    Ini f; memset(&f, 0, sizeof f);
    int have = !ini_host(GA(5), GPM_MODE_READ, host, sizeof host) && !ini_load(host, &f);
    if (dbg("ini")) fprintf(stderr, "DBG GetPrivateProfileStringA [%s] %s default='%s' file='%s' %s\n", app ? app : "(null)", key ? key : "(null)", def ? def : "", gstr(GA(5)) ? gstr(GA(5)) : "", have ? "" : "(no file)");
    if (!out || !size) { ini_free(&f); *r = 0; return GM_OK; }
    if (!app) {                                    /* all section names */
        Multi m = { gstr(out), size, 0, 0 };
        for (size_t i = 0; i < f.count; i++) { const char *nm; size_t nl; if (ini_header(f.line[i], &nm, &nl)) multi_add(&m, nm, nl); }
        *r = multi_end(&m);
    } else if (!key) {                             /* all key names of the section */
        Multi m = { gstr(out), size, 0, 0 };
        size_t a0, a1;
        if (ini_section(&f, app, &a0, &a1))
            for (size_t i = a0; i < a1; i++) { const char *k, *v; size_t kl; if (ini_keyline(f.line[i], &k, &kl, &v)) multi_add(&m, k, kl); }
        *r = multi_end(&m);
    } else {
        char value[4096];
        if (ini_find(&f, app, key, value, sizeof value)) *r = put_string(out, size, value);
        else {
            char d[4096]; size_t n = def ? strlen(def) : 0;
            if (n >= sizeof d) n = sizeof d - 1;
            if (n) memcpy(d, def, n);
            while (n && d[n - 1] == ' ') n--;      /* documented: trailing blanks of the default are dropped */
            d[n] = 0;
            *r = put_string(out, size, d);
        }
    }
    ini_free(&f);
    return GM_OK;
}
static int ob_get_private_profile_int_a(const uint32_t *a, uint32_t *r) {
    const char *app = gstr(GA(0)), *key = gstr(GA(1));
    char host[GPM_MAX_HOST], value[256];
    Ini f; memset(&f, 0, sizeof f);
    *r = GA(2);
    if (app && key && !ini_host(GA(3), GPM_MODE_READ, host, sizeof host) && !ini_load(host, &f) && ini_find(&f, app, key, value, sizeof value)) {
        const char *s = skip_ws(value);
        int neg = 0;
        if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
        uint32_t v = 0;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { for (s += 2; isxdigit((unsigned char)*s); s++) v = v * 16 + (uint32_t)(isdigit((unsigned char)*s) ? *s - '0' : (tolower((unsigned char)*s) - 'a' + 10)); }
        else for (; isdigit((unsigned char)*s); s++) v = v * 10 + (uint32_t)(*s - '0');
        *r = neg ? (uint32_t)(-(int32_t)v) : v;
    }
    if (dbg("ini")) fprintf(stderr, "DBG GetPrivateProfileIntA [%s] %s -> %d\n", app ? app : "(null)", key ? key : "(null)", (int32_t)*r);
    ini_free(&f);
    return GM_OK;
}
static int ob_write_private_profile_string_a(const uint32_t *a, uint32_t *r) {
    const char *app = gstr(GA(0)), *key = gstr(GA(1)), *val = gstr(GA(2));
    char host[GPM_MAX_HOST];
    if (dbg("ini")) fprintf(stderr, "DBG WritePrivateProfileStringA [%s] %s = %s file='%s'\n", app ? app : "(null)", key ? key : "(null)", val ? val : "(null)", gstr(GA(3)) ? gstr(GA(3)) : "");
    if (!app) { *r = 1; return GM_OK; }             /* cache flush request: nothing is cached */
    Ini f;
    if (ini_host(GA(3), GPM_MODE_WRITE, host, sizeof host) || ini_load(host, &f)) { set_last_error(3); *r = 0; return GM_OK; }
    size_t a0, a1;
    int have = ini_section(&f, app, &a0, &a1);
    if (!key) {                                     /* delete the whole section */
        if (have) for (size_t i = a1; i-- > a0 - 1;) { ini_remove(&f, i); if (i == 0) break; }
    } else {
        if (!have && val) {
            char hdr[1024]; snprintf(hdr, sizeof hdr, "[%s]", app);
            ini_push(&f, hdr, strlen(hdr), f.count);
            a0 = a1 = f.count; have = 1;
        }
        if (have) {
            size_t at = (size_t)-1;
            for (size_t i = a0; i < a1; i++) { const char *k, *v; size_t kl; if (ini_keyline(f.line[i], &k, &kl, &v) && span_ieq(k, kl, key)) { at = i; break; } }
            if (!val) { if (at != (size_t)-1) ini_remove(&f, at); }
            else {
                size_t n = strlen(key) + strlen(val) + 2;
                char *ln = malloc(n);
                if (!ln) { ini_free(&f); return GM_ALLOCATION; }
                snprintf(ln, n, "%s=%s", key, val);
                if (at != (size_t)-1) { free(f.line[at]); f.line[at] = ln; }
                else {
                    size_t ins = a1;                /* after the last non-blank line of the section */
                    while (ins > a0 && !*skip_ws(f.line[ins - 1])) ins--;
                    ini_push(&f, ln, strlen(ln), ins); free(ln);
                }
            }
        }
    }
    int rc = ini_save(host, &f);
    ini_free(&f);
    if (rc) { set_last_error(5); *r = 0; return GM_OK; }
    *r = 1;
    return GM_OK;
}
static int ob_get_private_profile_section_names_a(const uint32_t *a, uint32_t *r) {
    uint32_t b[6] = { 0, 0, 0, GA(0), GA(1), GA(2) };
    return ob_get_private_profile_string_a(b, r);
}
static int ob_get_private_profile_section_a(const uint32_t *a, uint32_t *r) {
    const char *app = gstr(GA(0));
    char host[GPM_MAX_HOST];
    Ini f; memset(&f, 0, sizeof f);
    if (!GA(1) || !GA(2)) { *r = 0; return GM_OK; }
    Multi m = { gstr(GA(1)), GA(2), 0, 0 };
    size_t a0, a1;
    if (app && !ini_host(GA(3), GPM_MODE_READ, host, sizeof host) && !ini_load(host, &f) && ini_section(&f, app, &a0, &a1))
        for (size_t i = a0; i < a1; i++) {
            const char *k, *v; size_t kl;
            if (!ini_keyline(f.line[i], &k, &kl, &v)) continue;
            char val[4096], ln[4400];
            ini_value(v, val, sizeof val);
            int n = snprintf(ln, sizeof ln, "%.*s=%s", (int)kl, k, val);
            multi_add(&m, ln, (size_t)n);
        }
    *r = multi_end(&m);
    ini_free(&f);
    return GM_OK;
}
static int ob_write_private_profile_section_a(const uint32_t *a, uint32_t *r) {
    const char *app = gstr(GA(0)), *data = gstr(GA(1));
    char host[GPM_MAX_HOST];
    Ini f;
    if (!app || ini_host(GA(2), GPM_MODE_WRITE, host, sizeof host) || ini_load(host, &f)) { set_last_error(3); *r = 0; return GM_OK; }
    size_t a0, a1;
    if (ini_section(&f, app, &a0, &a1)) { for (size_t i = a1; i > a0; i--) ini_remove(&f, i - 1); }
    else if (data) { char hdr[1024]; snprintf(hdr, sizeof hdr, "[%s]", app); ini_push(&f, hdr, strlen(hdr), f.count); a0 = f.count; }
    if (data) for (const char *s = data; *s; s += strlen(s) + 1) ini_push(&f, s, strlen(s), a0++);
    int rc = ini_save(host, &f);
    ini_free(&f);
    *r = rc ? 0u : 1u;
    if (rc) set_last_error(5);
    return GM_OK;
}

/* ================================================================== */
/* locale queries of the ANSI CRT (profile: en-US, CP1252 / OEM 437)    */
/* ================================================================== */
/* port/locale-profile.json: ACP 1252, OEMCP 437, user/system locale 0x0409. */
#define OB_LCID 0x0409u
static int lcid_known(uint32_t lcid) { return lcid == OB_LCID || lcid == 0x0400u || lcid == 0x0800u || lcid == 0u; }

static int ob_lcid(const uint32_t *a, uint32_t *r) { (void)a; *r = OB_LCID; return GM_OK; }
static int ob_get_oemcp(const uint32_t *a, uint32_t *r) { (void)a; *r = 437u; return GM_OK; }
static int ob_is_dbcs_lead_byte(const uint32_t *a, uint32_t *r) { (void)a; *r = 0u; return GM_OK; }   /* CP1252 has no lead bytes */
static int ob_is_valid_locale(const uint32_t *a, uint32_t *r) { *r = (GA(0) == OB_LCID) ? 1u : 0u; return GM_OK; }

static const struct { uint32_t type; const char *text; } ob_locale_info[] = {
    { 0x0001, "0409" },          /* LOCALE_ILANGUAGE */
    { 0x0002, "English (United States)" },   /* LOCALE_SLANGUAGE */
    { 0x0003, "ENU" },           /* LOCALE_SABBREVLANGNAME */
    { 0x0004, "English" },       /* LOCALE_SNATIVELANGNAME */
    { 0x0005, "1" },             /* LOCALE_ICOUNTRY */
    { 0x0006, "United States" }, /* LOCALE_SCOUNTRY */
    { 0x0007, "USA" },           /* LOCALE_SABBREVCTRYNAME */
    { 0x0008, "United States" }, /* LOCALE_SNATIVECTRYNAME */
    { 0x0009, "0409" },          /* LOCALE_IDEFAULTLANGUAGE */
    { 0x000A, "1" },             /* LOCALE_IDEFAULTCOUNTRY */
    { 0x000B, "437" },           /* LOCALE_IDEFAULTCODEPAGE */
    { 0x000C, "," },             /* LOCALE_SLIST */
    { 0x000D, "1" },             /* LOCALE_IMEASURE (US) */
    { 0x000E, "." },             /* LOCALE_SDECIMAL */
    { 0x000F, "," },             /* LOCALE_STHOUSAND */
    { 0x0010, "3;0" },           /* LOCALE_SGROUPING */
    { 0x0011, "2" },             /* LOCALE_IDIGITS */
    { 0x0012, "1" },             /* LOCALE_ILZERO */
    { 0x0013, "0123456789" },    /* LOCALE_SNATIVEDIGITS */
    { 0x0014, "$" },             /* LOCALE_SCURRENCY */
    { 0x0015, "USD" },           /* LOCALE_SINTLSYMBOL */
    { 0x0016, "." },             /* LOCALE_SMONDECIMALSEP */
    { 0x0017, "," },             /* LOCALE_SMONTHOUSANDSEP */
    { 0x0018, "3;0" },           /* LOCALE_SMONGROUPING */
    { 0x0019, "2" },             /* LOCALE_ICURRDIGITS */
    { 0x001A, "2" },             /* LOCALE_IINTLCURRDIGITS */
    { 0x001B, "0" },             /* LOCALE_ICURRENCY */
    { 0x001C, "0" },             /* LOCALE_INEGCURR */
    { 0x001D, "/" },             /* LOCALE_SDATE */
    { 0x001E, ":" },             /* LOCALE_STIME */
    { 0x001F, "M/d/yyyy" },      /* LOCALE_SSHORTDATE */
    { 0x0020, "dddd, MMMM d, yyyy" },  /* LOCALE_SLONGDATE */
    { 0x0021, "0" },             /* LOCALE_IDATE */
    { 0x0022, "0" },             /* LOCALE_ILDATE */
    { 0x0023, "0" },             /* LOCALE_ITIME */
    { 0x0024, "1" },             /* LOCALE_ICENTURY */
    { 0x0025, "0" },             /* LOCALE_ITLZERO */
    { 0x0026, "0" },             /* LOCALE_IDAYLZERO */
    { 0x0027, "0" },             /* LOCALE_IMONLZERO */
    { 0x0028, "AM" },            /* LOCALE_S1159 */
    { 0x0029, "PM" },            /* LOCALE_S2359 */
    { 0x002A, "Monday" }, { 0x002B, "Tuesday" }, { 0x002C, "Wednesday" }, { 0x002D, "Thursday" },
    { 0x002E, "Friday" }, { 0x002F, "Saturday" }, { 0x0030, "Sunday" },
    { 0x0031, "Mon" }, { 0x0032, "Tue" }, { 0x0033, "Wed" }, { 0x0034, "Thu" },
    { 0x0035, "Fri" }, { 0x0036, "Sat" }, { 0x0037, "Sun" },
    { 0x0038, "January" }, { 0x0039, "February" }, { 0x003A, "March" }, { 0x003B, "April" },
    { 0x003C, "May" }, { 0x003D, "June" }, { 0x003E, "July" }, { 0x003F, "August" },
    { 0x0040, "September" }, { 0x0041, "October" }, { 0x0042, "November" }, { 0x0043, "December" },
    { 0x0044, "Jan" }, { 0x0045, "Feb" }, { 0x0046, "Mar" }, { 0x0047, "Apr" },
    { 0x0048, "May" }, { 0x0049, "Jun" }, { 0x004A, "Jul" }, { 0x004B, "Aug" },
    { 0x004C, "Sep" }, { 0x004D, "Oct" }, { 0x004E, "Nov" }, { 0x004F, "Dec" },
    { 0x0050, "" },              /* LOCALE_SPOSITIVESIGN */
    { 0x0051, "-" },             /* LOCALE_SNEGATIVESIGN */
    /* en-US monetary layout: "$1.10" / "($1.10)" (matches ICURRENCY 0, INEGCURR 0) */
    { 0x0052, "3" },             /* LOCALE_IPOSSIGNPOSN */
    { 0x0053, "0" },             /* LOCALE_INEGSIGNPOSN: parentheses */
    { 0x0054, "1" },             /* LOCALE_IPOSSYMPRECEDES */
    { 0x0055, "0" },             /* LOCALE_IPOSSEPBYSPACE */
    { 0x0056, "1" },             /* LOCALE_INEGSYMPRECEDES */
    { 0x0057, "0" },             /* LOCALE_INEGSEPBYSPACE */
    { 0x0059, "en" },            /* LOCALE_SISO639LANGNAME */
    { 0x005A, "US" },            /* LOCALE_SISO3166CTRYNAME */
    { 0x1001, "English" },       /* LOCALE_SENGLANGUAGE */
    { 0x1002, "United States" }, /* LOCALE_SENGCOUNTRY */
    { 0x1003, "h:mm:ss tt" },    /* LOCALE_STIMEFORMAT */
    { 0x1004, "1252" },          /* LOCALE_IDEFAULTANSICODEPAGE */
    { 0x1005, "0" },             /* LOCALE_ITIMEMARKPOSN: AM/PM as suffix */
    { 0x1009, "1" },             /* LOCALE_ICALENDARTYPE */
    { 0x100B, "0" },             /* LOCALE_IOPTIONALCALENDAR */
    { 0x100C, "6" },             /* LOCALE_IFIRSTDAYOFWEEK (Sunday) */
    { 0x100D, "0" },             /* LOCALE_IFIRSTWEEKOFYEAR */
    { 0x1010, "1" },             /* LOCALE_INEGNUMBER: -1.1 */
    { 0x1011, "10000" },         /* LOCALE_IDEFAULTMACCODEPAGE */
    { 0x1012, "500" },           /* LOCALE_IDEFAULTEBCDICCODEPAGE */
};
static const char *locale_text(uint32_t type) {
    for (size_t i = 0; i < sizeof ob_locale_info / sizeof ob_locale_info[0]; i++)
        if (ob_locale_info[i].type == type) return ob_locale_info[i].text;
    return NULL;
}
static int locale_info(const uint32_t *a, uint32_t *r, int wide) {
    uint32_t lcid = GA(0), type = GA(1) & 0x0000FFFFu, flags = GA(1) & 0xFFFF0000u, out = GA(2), cch = GA(3);
    if (!lcid_known(lcid)) { set_last_error(87); *r = 0; return GM_OK; }
    const char *text = locale_text(type);
    if (!text) {
        fprintf(stderr, "PORT_NOTE GetLocaleInfo%c: LCTYPE 0x%04X not in the en-US profile table\n", wide ? 'W' : 'A', type);
        return GM_UNSUPPORTED;
    }
    if (flags & 0x20000000u) {                      /* LOCALE_RETURN_NUMBER */
        uint32_t v = (uint32_t)strtoul(text, NULL, type == 0x0001 || type == 0x0009 ? 16 : 10);
        uint32_t need = wide ? 2u : 4u;
        if (!cch) { *r = need; return GM_OK; }
        if (cch < need) { set_last_error(122); *r = 0; return GM_OK; }
        memcpy(gstr(out), &v, 4);
        *r = need;
        return GM_OK;
    }
    uint32_t n = (uint32_t)strlen(text) + 1;
    if (!cch) { *r = n; return GM_OK; }
    if (cch < n) { set_last_error(122); *r = 0; return GM_OK; }
    if (wide) { uint16_t *w = (uint16_t *)gstr(out); for (uint32_t i = 0; i < n; i++) w[i] = (uint8_t)text[i]; }
    else memcpy(gstr(out), text, n);
    *r = n;
    return GM_OK;
}
static int ob_get_locale_info_a(const uint32_t *a, uint32_t *r) { return locale_info(a, r, 0); }
static int ob_get_locale_info_w(const uint32_t *a, uint32_t *r) { return locale_info(a, r, 1); }

/* GetTimeZoneInformation: the port's clock policy is UTC (no bias, no DST). */
static int ob_get_time_zone_information(const uint32_t *a, uint32_t *r) {
    uint8_t tz[172];
    memset(tz, 0, sizeof tz);
    static const char name[] = "UTC";
    for (unsigned i = 0; i < 3; i++) { tz[4 + i * 2] = (uint8_t)name[i]; tz[88 + i * 2] = (uint8_t)name[i]; }
    if (gm_write(g_mem, GA(0), tz, sizeof tz)) return GM_UNCOMMITTED;
    *r = 0;                                         /* TIME_ZONE_ID_UNKNOWN: no daylight saving */
    return GM_OK;
}

/* ================================================================== */
/* drives, disk space, loader, console                                  */
/* ================================================================== */
static int ob_get_drive_type_a(const uint32_t *a, uint32_t *r) {
    const char *root = gstr(GA(0));
    /* Only C: exists in the projected file system (game + user folders). */
    if (!root || ((root[0] == 'C' || root[0] == 'c') && root[1] == ':')) *r = 3u;   /* DRIVE_FIXED */
    else *r = 1u;                                                                   /* DRIVE_NO_ROOT_DIR */
    if (dbg("drive")) fprintf(stderr, "DBG GetDriveTypeA('%s') -> %u\n", root ? root : "(null)", *r);
    return GM_OK;
}
static int ob_get_disk_free_space_ex_a(const uint32_t *a, uint32_t *r) {
    const char *dir = gstr(GA(0));
    char host[GPM_MAX_HOST];
    struct statvfs sv;
    const char *probe = dir && dir[0] ? dir : "C:\\BurnoutPR";
    if (gpm_resolve(g_pmap, probe, GPM_MODE_DIR, host, sizeof host) != GPM_OK || statvfs(host, &sv)) {
        /* fall back to the user folder root, which always exists */
        if (gpm_resolve(g_pmap, "C:\\Users\\Player", GPM_MODE_DIR, host, sizeof host) != GPM_OK || statvfs(host, &sv)) { set_last_error(3); *r = 0; return GM_OK; }
    }
    uint64_t avail = (uint64_t)sv.f_bavail * sv.f_frsize, total = (uint64_t)sv.f_blocks * sv.f_frsize, freeb = (uint64_t)sv.f_bfree * sv.f_frsize;
    if (GA(1) && gm_write(g_mem, GA(1), &avail, 8)) return GM_UNCOMMITTED;
    if (GA(2) && gm_write(g_mem, GA(2), &total, 8)) return GM_UNCOMMITTED;
    if (GA(3) && gm_write(g_mem, GA(3), &freeb, 8)) return GM_UNCOMMITTED;
    *r = 1;
    return GM_OK;
}
static int ob_load_library_ex_a(const uint32_t *a, uint32_t *r) {
    const char *name = gstr(GA(0));
    if (!name) { set_last_error(87); *r = 0; return GM_OK; }
    if (GA(1) || (GA(2) & ~0x8u)) {                 /* only plain loads (LOAD_WITH_ALTERED_SEARCH_PATH is a search hint) */
        fprintf(stderr, "PORT_NOTE LoadLibraryExA('%s', flags=0x%X): data-file/resource loads are not modelled\n", name, GA(2));
        return GM_UNSUPPORTED;
    }
    const char *base = strrchr(name, '\\');
    return static_load_library(base ? base + 1 : name, r);
}
/* FlushInstructionCache: nothing to flush in a static image, but it means the
 * guest just wrote code -- say so, the translation of that range may be stale. */
static int ob_flush_instruction_cache(const uint32_t *a, uint32_t *r) {
    fprintf(stderr, "PORT_NOTE FlushInstructionCache(base=0x%08X, size=0x%X): guest reports rewritten code\n", GA(1), GA(2));
    *r = 1;
    return GM_OK;
}
/* No console is attached to this GUI process. */
static int ob_get_console_mode(const uint32_t *a, uint32_t *r) { (void)a; set_last_error(6); *r = 0; return GM_OK; }
static int ob_get_console_cp(const uint32_t *a, uint32_t *r) { (void)a; *r = 0; return GM_OK; }
/* CreateProcessA: this runtime starts no child process (same documented refusal as CreateProcessW). */
static int ob_create_process_a(const uint32_t *a, uint32_t *r) {
    fprintf(stderr, "PORT_REFUSE CreateProcessA('%s', '%s') (no child processes)\n", gstr(GA(0)) ? gstr(GA(0)) : "", gstr(GA(1)) ? gstr(GA(1)) : "");
    set_last_error(2);
    *r = 0;
    return GM_OK;
}

/* ExtractIconA: window icons are not modelled (there is no window frame on the
 * target surface). NULL is the API's "no icon" answer; the caller keeps the
 * class's default icon. */
static int ob_extract_icon_a(const uint32_t *a, uint32_t *r) {
    static int noted;
    if (!noted++) fprintf(stderr, "PORT_NOTE ExtractIconA('%s', %d): icons are not modelled -> NULL\n", gstr(GA(1)) ? gstr(GA(1)) : "", (int32_t)GA(2));
    *r = 0;
    return GM_OK;
}

/* SHFileOperationA. The game uses one form (0x007CDA10): FO_DELETE of
 * "<dir>\*.*" with FOF_SILENT|FOF_NOCONFIRMATION|FOF_FILESONLY|FOF_NOERRORUI|
 * FOF_NORECURSION (0x1494), i.e. "delete the files directly in this folder";
 * it runs when graphics settings are applied. Modelled: FO_DELETE of files
 * (wildcards in the last component), never directories, never recursive, and
 * only below the projected user folders (C:\Users\..., C:\ProgramData\...):
 * the game folder holds the user's original data and is never deleted from;
 * a request there is refused as the API's access-denied answer. Every other
 * operation or flag set is a named stop. SHFILEOPSTRUCTA is packed on Win32:
 * hwnd 0, wFunc 4, pFrom 8, pTo 12, fFlags 16 (WORD), fAnyOperationsAborted 18,
 * hNameMappings 22, lpszProgressTitle 26. */
#include <dirent.h>
#include <fnmatch.h>
#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD (1 << 4)   /* GNU extension (same value in glibc, musl, bionic); glibc hides it without _GNU_SOURCE */
#endif
#include <sys/stat.h>
#include <unistd.h>
static int ob_user_area(const char *canonical) {
    return !strncasecmp(canonical, "C:\\Users\\", 9) || !strncasecmp(canonical, "C:\\ProgramData\\", 15);
}
static int ob_sh_file_operation_a(const uint32_t *a, uint32_t *r) {
    const uint8_t *op = (const uint8_t *)port_flat_base() + GA(0);
    if (!GA(0)) { *r = 0x57u; return GM_OK; }                    /* ERROR_INVALID_PARAMETER */
    uint32_t func, from, flags_word; uint16_t flags;
    memcpy(&func, op + 4, 4); memcpy(&from, op + 8, 4); memcpy(&flags, op + 16, 2); flags_word = flags;
    const uint32_t allowed = 0x0004u | 0x0010u | 0x0080u | 0x0400u | 0x1000u;   /* SILENT NOCONFIRMATION FILESONLY NOERRORUI NORECURSION */
    if (func != 3u || (flags_word & ~allowed) || !(flags_word & 0x0080u) || !from) {
        fprintf(stderr, "PORT_NOTE SHFileOperationA wFunc=%u fFlags=0x%X: only FO_DELETE of files is modelled\n", func, flags_word);
        return GM_UNSUPPORTED;
    }
    uint32_t result = 0, deleted = 0;
    for (const char *entry = gstr(from); entry && *entry; entry += strlen(entry) + 1) {
        char canonical[GPM_MAX_PATH], dir_guest[GPM_MAX_PATH], host_dir[GPM_MAX_HOST];
        if (gpm_normalize(entry, canonical, sizeof canonical) != GPM_OK) { result = 0x7Cu; break; }   /* DE_INVALIDFILES */
        if (!ob_user_area(canonical)) {
            fprintf(stderr, "PORT_NOTE SHFileOperationA delete '%s' refused: outside the user folders\n", canonical);
            result = 0x78u;                                             /* DE_ACCESSDENIEDSRC */
            break;
        }
        char *slash = strrchr(canonical, '\\');
        if (!slash) { result = 0x7Cu; break; }
        const char *pattern = slash + 1;
        snprintf(dir_guest, sizeof dir_guest, "%.*s", (int)(slash - canonical), canonical);
        if (gpm_resolve(g_pmap, dir_guest, GPM_MODE_DIR, host_dir, sizeof host_dir) != GPM_OK) { result = 0x7Cu; break; }
        int all = !strcmp(pattern, "*.*") || !strcmp(pattern, "*");
        DIR *d = opendir(host_dir);
        if (!d) { result = 0x7Cu; break; }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (!all && fnmatch(pattern, e->d_name, FNM_CASEFOLD)) continue;
            char host[GPM_MAX_HOST + 256];
            snprintf(host, sizeof host, "%s/%s", host_dir, e->d_name);
            struct stat st;
            if (lstat(host, &st) || !S_ISREG(st.st_mode)) continue;   /* FOF_FILESONLY + NORECURSION: files only */
            if (unlink(host)) { result = 0x78u; continue; }
            deleted++;
        }
        closedir(d);
    }
    uint32_t aborted = 0;
    memcpy((uint8_t *)op + 18, &aborted, 4);
    fprintf(stderr, "PORT_NOTE SHFileOperationA delete '%s': %u files removed, result %u\n", gstr(from), deleted, result);
    *r = result;
    return GM_OK;
}

/* ================================================================== */
/* user32 odds and ends around the one modelled game window             */
/* ================================================================== */
static int ob_set_rect(const uint32_t *a, uint32_t *r) {
    uint32_t rc[4] = { GA(1), GA(2), GA(3), GA(4) };
    if (!GA(0)) { *r = 0; return GM_OK; }
    if (gm_write(g_mem, GA(0), rc, 16)) return GM_UNCOMMITTED;
    *r = 1;
    return GM_OK;
}
/* FindWindowA: looks for another process's window (single-instance check).
 * There is no other process, hence no such window. */
static int ob_find_window_a(const uint32_t *a, uint32_t *r) { (void)a; *r = 0; return GM_OK; }
/* RegisterDeviceNotificationA: registration succeeds; this runtime never
 * broadcasts WM_DEVICECHANGE (controllers are reported through XInput state). */
static int ob_register_device_notification_a(const uint32_t *a, uint32_t *r) { (void)a; *r = 0x00DE0001u; return GM_OK; }
static int ob_unhook_windows_hook_ex(const uint32_t *a, uint32_t *r) { (void)a; *r = 1; return GM_OK; }
/* SystemParametersInfoA: the accessibility-key and screen-saver switches games
 * read and restore.  Nothing of the kind is active on the target, a "set" has
 * nothing to change.  Any other action is a named stop. */
static int ob_system_parameters_info_a(const uint32_t *a, uint32_t *r) {
    uint32_t action = GA(0), out = GA(2);
    switch (action) {
    case 0x0032: case 0x0034: case 0x003A: {        /* GET FILTERKEYS / TOGGLEKEYS / STICKYKEYS */
        uint32_t cb = 0;
        if (!out || gm_read(g_mem, out, &cb, 4)) { *r = 0; return GM_OK; }
        uint32_t zero = 0;                          /* dwFlags = 0: feature off, hotkey off */
        if (gm_write(g_mem, out + 4, &zero, 4)) return GM_UNCOMMITTED;
        if (action == 0x0032 && cb >= 24) { uint32_t z[4] = {0, 0, 0, 0}; if (gm_write(g_mem, out + 8, z, 16)) return GM_UNCOMMITTED; }
        *r = 1; return GM_OK;
    }
    case 0x0033: case 0x0035: case 0x003B:          /* SET FILTERKEYS / TOGGLEKEYS / STICKYKEYS */
    case 0x0011:                                    /* SPI_SETSCREENSAVEACTIVE */
        *r = 1; return GM_OK;
    case 0x0010: {                                  /* SPI_GETSCREENSAVEACTIVE -> FALSE */
        uint32_t zero = 0;
        if (out && gm_write(g_mem, out, &zero, 4)) return GM_UNCOMMITTED;
        *r = 1; return GM_OK;
    }
    default:
        fprintf(stderr, "PORT_NOTE SystemParametersInfoA: action 0x%04X is not modelled\n", action);
        return GM_UNSUPPORTED;
    }
}

/* RegisterWindowMessageA: process-wide message atoms in 0xC000..0xFFFF. */
static int ob_register_window_message_a(const uint32_t *a, uint32_t *r) {
    static char names[64][64]; static uint32_t count;
    const char *name = gstr(GA(0));
    if (!name || !name[0]) { set_last_error(87); *r = 0; return GM_OK; }
    for (uint32_t i = 0; i < count; i++) if (!strcasecmp(names[i], name)) { *r = 0xC000u + i; return GM_OK; }
    if (count == 64 || strlen(name) >= sizeof names[0]) return GM_UNSUPPORTED;
    strcpy(names[count], name);
    *r = 0xC000u + count++;
    return GM_OK;
}
/* The one game window is the foreground, focus and active window while it exists. */
uint32_t gu_hwnd(GuestUser32 *user32);
static uint32_t ob_capture;
static int ob_main_hwnd(const uint32_t *a, uint32_t *r) { (void)a; *r = g_user32 ? gu_hwnd(g_user32) : 0; return GM_OK; }
static int ob_set_focus(const uint32_t *a, uint32_t *r) { (void)a; *r = g_user32 ? gu_hwnd(g_user32) : 0; return GM_OK; }   /* previous focus */
static int ob_set_capture(const uint32_t *a, uint32_t *r) { *r = ob_capture; ob_capture = GA(0); return GM_OK; }
static int ob_release_capture(const uint32_t *a, uint32_t *r) { (void)a; ob_capture = 0; *r = 1; return GM_OK; }
static int ob_is_window(const uint32_t *a, uint32_t *r) { *r = (g_user32 && GA(0) && GA(0) == gu_hwnd(g_user32)) ? 1u : 0u; return GM_OK; }
static int ob_zero_1(const uint32_t *a, uint32_t *r) { (void)a; *r = 0; return GM_OK; }
static int ob_true(const uint32_t *a, uint32_t *r) { (void)a; *r = 1; return GM_OK; }
/* SetWindowPos/MoveWindow: the window is the device surface; its position and
 * size are fixed by the host, so a placement request has nothing to move. */
static int ob_window_placement(const uint32_t *a, uint32_t *r) {
    static int noted;
    if (!noted++) fprintf(stderr, "PORT_NOTE window placement request (hwnd=0x%X): the surface size is host-defined, request acknowledged\n", GA(0));
    *r = 1;
    return GM_OK;
}
/* Client origin == screen origin for the single full-surface window. */
static int ob_point_identity(const uint32_t *a, uint32_t *r) { *r = GA(1) ? 1u : 0u; return GM_OK; }
static int ob_char_next_a(const uint32_t *a, uint32_t *r) {
    const char *p = gstr(GA(0));
    *r = (p && *p) ? GA(0) + 1 : GA(0);             /* CP1252: single-byte characters */
    return GM_OK;
}
int port_guest_stdcall(uint32_t va, const uint32_t *args, uint32_t count, uint32_t *result);
static int ob_call_window_proc_a(const uint32_t *a, uint32_t *r) {
    if (!GA(0)) { *r = 0; return GM_OK; }
    uint32_t args[4] = { GA(1), GA(2), GA(3), GA(4) };
    port_guest_stdcall(GA(0), args, 4, r);          /* an exception leaving the callee sets the abandon flag */
    return GM_OK;
}

/* D3DPERF_GetStatus: non-zero only while a PIX profiler is attached. None is. */
static int ob_d3dperf_get_status(const uint32_t *a, uint32_t *r) { (void)a; *r = 0; return GM_OK; }

/* Direct3DCreate9: the real DXVK object behind a guest COM object
 * (runtime/d3d9_thunk.cpp). NULL when DXVK cannot be loaded -- which the game
 * reports itself, exactly as with a missing d3d9.dll feature level. */
static int ob_direct3d_create9(const uint32_t *a, uint32_t *r) { *r = d3d9t_direct3d_create9(GA(0)); return GM_OK; }

/* ================================================================== */
/* PE resources of the game image                                      */
/* ================================================================== */
/* FindResource/LoadResource/LockResource/SizeofResource on the mapped image:
 * the three-level resource directory (type / name / language) is read from
 * guest memory exactly as the loader maps it. HRSRC is the address of the
 * IMAGE_RESOURCE_DATA_ENTRY, HGLOBAL the address of the data -- the values
 * Windows returns for a module mapped as an image. */
extern uint32_t g_image_base;
static uint32_t rd32g(uint32_t va) { uint32_t v; memcpy(&v, port_flat_base() + va, 4); return v; }
static uint32_t rd16g(uint32_t va) { uint16_t v; memcpy(&v, port_flat_base() + va, 2); return v; }
static int rsrc_root(uint32_t module, uint32_t *root, uint32_t *size) {
    uint32_t base = module ? module : g_image_base;
    if (base != g_image_base) return -1;
    if (rd16g(base) != 0x5A4D) return -1;
    uint32_t nt = base + rd32g(base + 0x3C);
    if (rd32g(nt) != 0x00004550u || rd16g(nt + 24) != 0x10B) return -1;
    if (rd32g(nt + 24 + 92) < 3) return -1;
    *root = rd32g(nt + 24 + 96 + 16); *size = rd32g(nt + 24 + 96 + 20);
    return (*root && *size) ? 0 : -1;
}
/* An id (MAKEINTRESOURCE or "#123") or a name; returns 1 when `key` is an id. */
static int rsrc_key(uint32_t key, uint32_t *id, const char **name) {
    if (key < 0x10000u) { *id = key; return 1; }
    const char *text = gstr(key);
    if (text[0] == '#') { *id = (uint32_t)strtoul(text + 1, NULL, 10); return 1; }
    *name = text;
    return 0;
}
/* Entry of directory `dir` (offset from the root) matching the key; returns the
 * entry's OffsetToData field or 0. */
static uint32_t rsrc_find(uint32_t root, uint32_t size, uint32_t dir, uint32_t key, int any) {
    if (dir + 16 > size) return 0;
    uint32_t named = rd16g(root + dir + 12), ids = rd16g(root + dir + 14);
    if (dir + 16 + (named + ids) * 8 > size) return 0;
    uint32_t id = 0; const char *name = NULL;
    int is_id = any ? 1 : rsrc_key(key, &id, &name);
    for (uint32_t i = 0; i < named + ids; i++) {
        uint32_t entry = root + dir + 16 + i * 8, e_name = rd32g(entry), e_data = rd32g(entry + 4);
        if (any) return e_data;
        if (e_name & 0x80000000u) {
            if (is_id) continue;
            uint32_t s = (e_name & 0x7FFFFFFFu);
            if (s + 2 > size) continue;
            uint32_t length = rd16g(root + s);
            if (s + 2 + length * 2 > size || strlen(name) != length) continue;
            uint32_t k = 0;
            for (; k < length; k++) if (toupper((unsigned char)name[k]) != (int)rd16g(root + s + 2 + k * 2)) break;
            if (k == length) return e_data;
        } else if (is_id && e_name == id) return e_data;
    }
    return 0;
}
static int ob_find_resource_a(const uint32_t *a, uint32_t *r) {    /* (hModule, lpName, lpType) */
    uint32_t root, size;
    *r = 0;
    if (rsrc_root(GA(0), &root, &size)) { set_last_error(1812); return GM_OK; }   /* ERROR_RESOURCE_DATA_NOT_FOUND */
    root += g_image_base;
    uint32_t type = rsrc_find(root, size, 0, GA(2), 0);
    if (!(type & 0x80000000u)) { set_last_error(1813); return GM_OK; }            /* ERROR_RESOURCE_TYPE_NOT_FOUND */
    uint32_t name = rsrc_find(root, size, type & 0x7FFFFFFFu, GA(1), 0);
    if (!(name & 0x80000000u)) { set_last_error(1814); return GM_OK; }            /* ERROR_RESOURCE_NAME_NOT_FOUND */
    uint32_t lang = rsrc_find(root, size, name & 0x7FFFFFFFu, 0, 1);              /* first language */
    if (!lang || (lang & 0x80000000u) || lang + 16 > size) { set_last_error(1815); return GM_OK; }
    *r = root + lang;
    return GM_OK;
}
static int rsrc_entry(uint32_t module, uint32_t hres, uint32_t *data, uint32_t *bytes) {
    uint32_t root, size;
    if (!hres || rsrc_root(module, &root, &size)) return -1;
    root += g_image_base;
    if (hres < root || hres + 16 > root + size) return -1;
    *data = g_image_base + rd32g(hres); *bytes = rd32g(hres + 4);
    return 0;
}
static int ob_load_resource(const uint32_t *a, uint32_t *r) {      /* (hModule, hResInfo) */
    uint32_t data, bytes;
    if (rsrc_entry(GA(0), GA(1), &data, &bytes)) { *r = 0; set_last_error(6); return GM_OK; }
    *r = data;
    return GM_OK;
}
static int ob_lock_resource(const uint32_t *a, uint32_t *r) { *r = GA(0); return GM_OK; }
static int ob_sizeof_resource(const uint32_t *a, uint32_t *r) {
    uint32_t data, bytes;
    if (rsrc_entry(GA(0), GA(1), &data, &bytes)) { *r = 0; set_last_error(6); return GM_OK; }
    *r = bytes;
    return GM_OK;
}
static int ob_free_resource(const uint32_t *a, uint32_t *r) { (void)a; *r = 0; return GM_OK; }

/* Secur32 (SSPI). There is no security support provider on this platform:
 * AcquireCredentialsHandle reports the status Windows gives for a package that
 * is not installed, SEC_E_SECPKG_NOT_FOUND. The game's TLS layer then has no
 * credentials and its online features stay unavailable (the servers of this
 * 2009 title are gone anyway). No handle is fabricated. */
static int ob_acquire_credentials_handle_a(const uint32_t *a, uint32_t *r) {
    const char *package = gstr(GA(1));
    static int noted;
    if (!noted++) fprintf(stderr, "PORT_NOTE Secur32!AcquireCredentialsHandleA(package='%s'): no SSP on this platform -> SEC_E_SECPKG_NOT_FOUND\n", package ? package : "");
    *r = 0x80090305u;
    return GM_OK;
}

/* EnumSystemLocalesA(lpLocaleEnumProc, dwFlags): the locale profile of this
 * runtime has exactly one installed locale, en-US (0409) -- the same profile
 * GetLocaleInfo/GetUserDefaultLCID answer from. The callback gets the LCID as
 * eight hex digits and may stop the enumeration by returning FALSE. */
uint32_t d3d9t_guest_alloc(uint32_t size);
static int ob_enum_system_locales_a(const uint32_t *a, uint32_t *r) {
    if (!GA(0)) { set_last_error(87); *r = 0; return GM_OK; }
    if (GA(1) & ~0x3u) { set_last_error(1004); *r = 0; return GM_OK; }    /* ERROR_INVALID_FLAGS */
    static RECOMP_TLS uint32_t scratch;
    if (!scratch) scratch = d3d9t_guest_alloc(16);
    if (!scratch) return GM_ALLOCATION;
    memcpy(port_flat_base() + scratch, "00000409", 9);
    uint32_t args[1] = { scratch }, keep_going = 0;
    port_guest_stdcall(GA(0), args, 1, &keep_going);
    *r = 1;
    return GM_OK;
}

/* CompareStringW(Locale, dwCmpFlags, lpString1, cchCount1, lpString2, cchCount2)
 * with the default sort of the installed sort NLS (sortdefault.nls): every
 * UTF-16 unit has four weights there -- { primary, script, diacritic, case }.
 * The comparison is the layered one Windows documents: all alphabetic
 * (script + primary) weights first, then the diacritic weights, then the case
 * weights; in a word sort (no SORT_STRINGSORT) hyphen/apostrophe-class
 * punctuation is taken out of the first pass and only breaks ties at the end.
 * en-US uses the default sort without compression or exception tables.
 *
 * Covered: unsortable units, non-spacing marks, punctuation, symbols, digits,
 * Latin, Greek and Cyrillic. Expansions (ae-ligature, sharp s ...), Hangul,
 * kana and CJK weights need more of the table than is read here: named stop.
 * Format reference only: wine dlls/kernelbase/locale.c (sort.keys layout). */
static uint8_t *ob_sort_keys;                       /* 65536 x 4 bytes */
void ob_install_sort_keys(const uint8_t *nls, size_t size) {
    uint32_t offset;
    if (size < 16) return;
    memcpy(&offset, nls, 4);
    if ((size_t)offset + 65536u * 4u > size) return;
    free(ob_sort_keys);
    ob_sort_keys = malloc(65536u * 4u);
    if (ob_sort_keys) memcpy(ob_sort_keys, nls + offset, 65536u * 4u);
}
typedef struct { uint8_t primary[2048], diacritic[1024], kase[1024], special[2048]; uint32_t np, nd, nc, ns; } ObSortKey;
static int ob_sort_key(const uint16_t *text, int32_t count, uint32_t flags, ObSortKey *key) {
    uint8_t case_mask = 0x3F;
    if (flags & 1u) case_mask &= (uint8_t)~(0x10 | 0x08);    /* NORM_IGNORECASE: upper, sub/superscript */
    key->np = key->nd = key->nc = key->ns = 0;
    for (int32_t i = 0; count < 0 ? text[i] != 0 : i < count; i++) {
        const uint8_t *w = ob_sort_keys + (uint32_t)text[i] * 4u;
        uint8_t primary = w[0], script = w[1], diacritic = w[2], kase = (uint8_t)(w[3] & case_mask);
        if (key->np + 2 > sizeof key->primary || key->nd + 1 > sizeof key->diacritic || key->ns + 4 > sizeof key->special) return -2;
        if (script == 0) continue;                           /* unsortable */
        if (script == 1) {                                   /* non-spacing mark: adds to the previous diacritic weight */
            if (key->nd) key->diacritic[key->nd - 1] = (uint8_t)(key->diacritic[key->nd - 1] + diacritic);
            else key->diacritic[key->nd++] = diacritic;
            continue;
        }
        if (script == 6 && !(flags & 0x1000u)) {             /* punctuation in a word sort: position + weight, compared last */
            int16_t position = (int16_t)(-(int32_t)(key->np / 2) - 1);
            key->special[key->ns++] = (uint8_t)((uint16_t)position >> 8); key->special[key->ns++] = (uint8_t)position;
            key->special[key->ns++] = primary; key->special[key->ns++] = (uint8_t)(kase | (diacritic << 3));
            continue;
        }
        if (script < 6 || script > 16) return -1;            /* expansion, East Asian, Hangul ... */
        key->primary[key->np++] = script; key->primary[key->np++] = primary;
        key->diacritic[key->nd++] = diacritic; key->kase[key->nc++] = kase;
    }
    while (key->nd && key->diacritic[key->nd - 1] <= 2) key->nd--;   /* trailing "no diacritic" / "no case" weights carry nothing */
    while (key->nc && key->kase[key->nc - 1] <= 2) key->nc--;
    return 0;
}
static int ob_key_compare(const uint8_t *x, uint32_t nx, const uint8_t *y, uint32_t ny) {
    int result = memcmp(x, y, nx < ny ? nx : ny);
    return result ? result : (int)nx - (int)ny;
}
static int ob_compare_string_w(const uint32_t *a, uint32_t *r) {
    uint32_t flags = GA(1);
    if (!GA(2) || !GA(4) || (int32_t)GA(3) < -1 || (int32_t)GA(5) < -1) { set_last_error(87); *r = 0; return GM_OK; }
    if (flags & ~0x00001001u) {                     /* NORM_IGNORECASE | SORT_STRINGSORT */
        fprintf(stderr, "PORT_NOTE CompareStringW flags 0x%08X are not modelled\n", flags);
        return GM_UNSUPPORTED;
    }
    if (!ob_sort_keys) { fprintf(stderr, "PORT_NOTE CompareStringW: no sort NLS installed\n"); return GM_UNSUPPORTED; }
    static RECOMP_TLS ObSortKey key[2];
    const uint16_t *s[2] = { (const uint16_t *)gstr(GA(2)), (const uint16_t *)gstr(GA(4)) };
    int32_t n[2] = { (int32_t)GA(3), (int32_t)GA(5) };
    for (int k = 0; k < 2; k++) {
        int rc = ob_sort_key(s[k], n[k], flags, &key[k]);
        if (rc) {
            fprintf(stderr, "PORT_NOTE CompareStringW operand %d %s\n", k, rc == -1 ? "contains a unit outside the modelled scripts (expansion/East Asian)" : "is longer than the key buffers");
            return GM_UNSUPPORTED;
        }
    }
    int result = ob_key_compare(key[0].primary, key[0].np, key[1].primary, key[1].np);
    if (!result) result = ob_key_compare(key[0].diacritic, key[0].nd, key[1].diacritic, key[1].nd);
    if (!result) result = ob_key_compare(key[0].kase, key[0].nc, key[1].kase, key[1].nc);
    if (!result) result = ob_key_compare(key[0].special, key[0].ns, key[1].special, key[1].ns);
    *r = result < 0 ? 1u : (result > 0 ? 3u : 2u);  /* CSTR_LESS_THAN / CSTR_EQUAL / CSTR_GREATER_THAN */
    return GM_OK;
}

/* Exports that are not present here (see orig_bridges.h).
 *   gdi32!D3DKMTOpenAdapterFromHdc / D3DKMTEscape: the Vista+ kernel-mode
 *   thunks the game uses for a vendor driver escape (multi-GPU query, function
 *   0x994590). There is no WDDM kernel driver on this platform; like on
 *   Windows XP the two exports are missing and the game skips the query. */
int orig_export_absent(const char *dll, const char *name) {
    static const struct { const char *dll, *name; } absent[] = {
        { "gdi32.dll", "D3DKMTOpenAdapterFromHdc" },
        { "gdi32.dll", "D3DKMTEscape" },
    };
    for (size_t i = 0; i < sizeof absent / sizeof absent[0]; i++)
        if (!strcasecmp(dll, absent[i].dll) && !strcmp(name, absent[i].name)) return 1;
    return 0;
}

/* d3dx9_37.dll: the shader compiler entry points (runtime/d3dx_thunk.cpp). */
static int ob_d3dx_compile_shader(const uint32_t *a, uint32_t *r) {
    if (!d3dxt_compile_shader(a, r)) return GM_OK;
    fprintf(stderr, "PORT_D3DX_STOP %s\n", d3dxt_last_stop());
    return GM_UNSUPPORTED;
}
static int ob_d3dx_get_shader_constant_table(const uint32_t *a, uint32_t *r) {
    if (!d3dxt_get_shader_constant_table(a, r)) return GM_OK;
    fprintf(stderr, "PORT_D3DX_STOP %s\n", d3dxt_last_stop());
    return GM_UNSUPPORTED;
}

const OrigBridge orig_bridges[] = {
    { "d3d9.dll", "D3DPERF_GetStatus", 0, ob_d3dperf_get_status },
    { "secur32.dll", "AcquireCredentialsHandleA", 36, ob_acquire_credentials_handle_a },
    { "kernel32.dll", "EnumSystemLocalesA", 8, ob_enum_system_locales_a },
    { "kernel32.dll", "CompareStringW", 24, ob_compare_string_w },
    { "kernel32.dll", "FindResourceA", 12, ob_find_resource_a },
    { "kernel32.dll", "LoadResource", 8, ob_load_resource },
    { "kernel32.dll", "LockResource", 4, ob_lock_resource },
    { "kernel32.dll", "SizeofResource", 8, ob_sizeof_resource },
    { "kernel32.dll", "FreeResource", 4, ob_free_resource },
    { "d3dx9_37.dll", "D3DXCompileShader", 40, ob_d3dx_compile_shader },
    { "dsound.dll", "GetDeviceID", 8, dsoundt_get_device_id },
    { "dsound.dll", "DirectSoundCreate", 12, dsoundt_create },
    { "dsound.dll", "DirectSoundCreate8", 12, dsoundt_create },
    { "d3dx9_37.dll", "D3DXGetShaderConstantTable", 8, ob_d3dx_get_shader_constant_table },
    { "d3d9.dll", "Direct3DCreate9", 4, ob_direct3d_create9 },
    { "shell32.dll", "ExtractIconA", 12, ob_extract_icon_a },
    { "shell32.dll", "SHFileOperationA", 4, ob_sh_file_operation_a },
    { "user32.dll", "SetRect", 20, ob_set_rect },
    { "user32.dll", "FindWindowA", 8, ob_find_window_a },
    { "user32.dll", "RegisterDeviceNotificationA", 12, ob_register_device_notification_a },
    { "user32.dll", "UnhookWindowsHookEx", 4, ob_unhook_windows_hook_ex },
    { "user32.dll", "SystemParametersInfoA", 16, ob_system_parameters_info_a },
    { "user32.dll", "RegisterWindowMessageA", 4, ob_register_window_message_a },
    { "user32.dll", "GetForegroundWindow", 0, ob_main_hwnd },
    { "user32.dll", "GetFocus", 0, ob_main_hwnd },
    { "user32.dll", "SetFocus", 4, ob_set_focus },
    { "user32.dll", "SetCapture", 4, ob_set_capture },
    { "user32.dll", "ReleaseCapture", 0, ob_release_capture },
    { "user32.dll", "IsWindow", 4, ob_is_window },
    { "user32.dll", "GetParent", 4, ob_zero_1 },
    { "user32.dll", "SetWindowPos", 28, ob_window_placement },
    { "user32.dll", "MoveWindow", 24, ob_window_placement },
    { "user32.dll", "SetWindowTextA", 8, ob_true },
    { "user32.dll", "SetWindowTextW", 8, ob_true },
    { "user32.dll", "LockSetForegroundWindow", 4, ob_true },
    { "user32.dll", "InvalidateRect", 12, ob_true },
    { "user32.dll", "ClientToScreen", 8, ob_point_identity },
    { "user32.dll", "ScreenToClient", 8, ob_point_identity },
    { "user32.dll", "CharNextA", 4, ob_char_next_a },
    { "user32.dll", "CallWindowProcA", 20, ob_call_window_proc_a },
    { "kernel32.dll", "GetPrivateProfileStringA", 24, ob_get_private_profile_string_a },
    { "kernel32.dll", "GetPrivateProfileIntA", 16, ob_get_private_profile_int_a },
    { "kernel32.dll", "WritePrivateProfileStringA", 16, ob_write_private_profile_string_a },
    { "kernel32.dll", "GetPrivateProfileSectionNamesA", 12, ob_get_private_profile_section_names_a },
    { "kernel32.dll", "GetPrivateProfileSectionA", 16, ob_get_private_profile_section_a },
    { "kernel32.dll", "WritePrivateProfileSectionA", 12, ob_write_private_profile_section_a },
    { "kernel32.dll", "GetThreadLocale", 0, ob_lcid },
    { "kernel32.dll", "GetUserDefaultLCID", 0, ob_lcid },
    { "kernel32.dll", "GetOEMCP", 0, ob_get_oemcp },
    { "kernel32.dll", "IsDBCSLeadByte", 4, ob_is_dbcs_lead_byte },
    { "kernel32.dll", "IsValidLocale", 8, ob_is_valid_locale },
    { "kernel32.dll", "GetLocaleInfoA", 16, ob_get_locale_info_a },
    { "kernel32.dll", "GetLocaleInfoW", 16, ob_get_locale_info_w },
    { "kernel32.dll", "GetTimeZoneInformation", 4, ob_get_time_zone_information },
    { "kernel32.dll", "GetDriveTypeA", 4, ob_get_drive_type_a },
    { "kernel32.dll", "GetDiskFreeSpaceExA", 16, ob_get_disk_free_space_ex_a },
    { "kernel32.dll", "LoadLibraryExA", 12, ob_load_library_ex_a },
    { "kernel32.dll", "FlushInstructionCache", 12, ob_flush_instruction_cache },
    { "kernel32.dll", "GetConsoleMode", 8, ob_get_console_mode },
    { "kernel32.dll", "GetConsoleCP", 0, ob_get_console_cp },
    { "kernel32.dll", "GetConsoleOutputCP", 0, ob_get_console_cp },
    { "kernel32.dll", "CreateProcessA", 40, ob_create_process_a },
};
const uint32_t orig_bridge_count = sizeof orig_bridges / sizeof orig_bridges[0];
