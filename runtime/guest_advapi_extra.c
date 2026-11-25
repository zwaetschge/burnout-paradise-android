/* advapi32 write overlay + crypto bridges.
 * See guest_advapi_extra.h for the modelled scope and the named limits. */
#include "guest_advapi_extra.h"
#include "guest_handles.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

typedef struct {
    char *name;
    uint32_t type;
    uint8_t *data;
    uint32_t size;
} GaxValue;

typedef struct {
    char *path;            /* "HKCU\\Software\\Vendor\\Game" */
    GaxValue *values;
    uint32_t value_count, value_capacity;
} GaxKey;

typedef struct {
    char *path;            /* owned copy, survives key deletion */
    uint32_t magic;
} GaxKeyRef;

typedef struct {
    uint32_t provider_type;
    uint32_t magic;
} GaxProvider;

#define GAX_KEY_MAGIC 0x4b584147u  /* "GAXK" */
#define GAX_PROV_MAGIC 0x50584147u /* "GAXP" */

struct GuestAdvapiExtra {
    GuestMemory *memory;
    uint32_t teb;
    GuestRegistry *registry;
    const GuestLocale *locale;
    char overlay_path[1024];
    uint32_t scratch, scratch_size;
    GaxKey *keys;
    uint32_t key_count, key_capacity;
    uint32_t persist_writes, random_bytes, provider_count;
    char last_stop[192];
};

static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}
static int write_u32(GuestAdvapiExtra *gax, uint32_t address, uint32_t value) {
    uint8_t b[4];
    put32(b, value);
    return gm_write(gax->memory, address, b, 4);
}
static int last_error(GuestAdvapiExtra *gax, uint32_t value) {
    return write_u32(gax, gax->teb + 0x34, value);
}
static int stop(GuestAdvapiExtra *gax, int code, const char *what) {
    snprintf(gax->last_stop, sizeof(gax->last_stop), "%s", what);
    fprintf(stderr, "advapi-extra-stop: %s (%d)\n", what, code);
    return code;
}

/* --- small helpers ------------------------------------------------------ */
static char *dup_string(const char *text) {
    size_t n = strlen(text) + 1;
    char *copy = (char *)malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}
static int fold(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}
/* Key paths compare case-insensitively and without trailing backslashes:
 * RegOpenKeyEx("Software\\X\\") opens the same key as "Software\\X". */
static int path_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (fold((unsigned char)*a) != fold((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    while (*a == '\\') a++;
    while (*b == '\\') b++;
    return *a == 0 && *b == 0;
}

static int overlay_load(GuestAdvapiExtra *gax);

int gax_create(GuestMemory *memory, uint32_t teb, GuestRegistry *registry,
               const GuestLocale *locale, const char *user_root,
               GuestAdvapiExtra **out) {
    if (!memory || !out || !user_root) return GM_RANGE;
    GuestAdvapiExtra *gax = (GuestAdvapiExtra *)calloc(1, sizeof(*gax));
    if (!gax) return GM_ALLOCATION;
    gax->memory = memory;
    gax->teb = teb;
    gax->registry = registry;
    gax->locale = locale;
    snprintf(gax->overlay_path, sizeof(gax->overlay_path),
             "%s/registry-overlay.json", user_root);
    /* A missing file is the normal first start; a corrupt one is reported in
     * last_stop and the overlay simply starts empty. */
    int rc = overlay_load(gax);
    if (rc != GM_OK && rc != GM_ALLOCATION)
        snprintf(gax->last_stop, sizeof(gax->last_stop),
                 "overlay file not readable, starting empty");
    *out = gax;
    return GM_OK;
}
void gax_destroy(GuestAdvapiExtra *gax) {
    if (!gax) return;
    for (uint32_t i = 0; i < gax->key_count; i++) {
        GaxKey *key = &gax->keys[i];
        for (uint32_t v = 0; v < key->value_count; v++) {
            free(key->values[v].name);
            free(key->values[v].data);
        }
        free(key->values);
        free(key->path);
    }
    free(gax->keys);
    free(gax);
}
/* Host side: a REG_SZ value the installer of the game writes, set before the
 * guest starts (not persisted; the setting it comes from is the source). */
static GaxKey *overlay_ensure(GuestAdvapiExtra *gax, const char *path);
static GaxValue *value_ensure(GaxKey *key, const char *name);
int gax_install_string(GuestAdvapiExtra *gax, const char *path, const char *name,
                       const char *ascii) {
    if (!gax || !path || !name || !ascii) return GM_RANGE;
    GaxKey *key = overlay_ensure(gax, path);
    if (!key) return GM_ALLOCATION;
    GaxValue *value = value_ensure(key, name);
    if (!value) return GM_ALLOCATION;
    uint32_t units = (uint32_t)strlen(ascii) + 1u;      /* with the terminating NUL */
    uint8_t *data = (uint8_t *)malloc(units * 2u);
    if (!data) return GM_ALLOCATION;
    for (uint32_t i = 0; i < units; i++) { data[i * 2] = (uint8_t)ascii[i]; data[i * 2 + 1] = 0; }
    free(value->data);
    value->data = data;
    value->size = units * 2u;
    value->type = GAX_REG_SZ;
    return GM_OK;
}
int gax_set_scratch(GuestAdvapiExtra *gax, uint32_t address, uint32_t size) {
    if (!gax || size < GAX_SCRATCH_MIN) return GM_UNSUPPORTED;
    int rc = gm_check(gax->memory, address, size, GM_WRITE);
    if (rc) return rc;
    gax->scratch = address;
    gax->scratch_size = size;
    return GM_OK;
}
uint32_t gax_overlay_keys(const GuestAdvapiExtra *gax) {
    return gax ? gax->key_count : 0u;
}
uint32_t gax_overlay_values(const GuestAdvapiExtra *gax) {
    if (!gax) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < gax->key_count; i++) n += gax->keys[i].value_count;
    return n;
}
uint32_t gax_persist_writes(const GuestAdvapiExtra *gax) {
    return gax ? gax->persist_writes : 0u;
}
uint32_t gax_providers(const GuestAdvapiExtra *gax) {
    return gax ? gax->provider_count : 0u;
}
uint32_t gax_random_bytes(const GuestAdvapiExtra *gax) {
    return gax ? gax->random_bytes : 0u;
}
const char *gax_overlay_path(const GuestAdvapiExtra *gax) {
    return gax ? gax->overlay_path : "";
}
const char *gax_last_stop(const GuestAdvapiExtra *gax) {
    return (gax && gax->last_stop[0]) ? gax->last_stop : NULL;
}
const char *gax_stop_name(int code) {
    switch (code) {
    case 0: return "ok";
    case 1: return "gm-range";
    case 2: return "gm-uncommitted";
    case 3: return "gm-allocation";
    case 4: return "gm-access";
    case 5: return "gm-unsupported";
    case 6: return "gm-overlap";
    case GU_STOP_GAX_NO_SCRATCH: return "advapi-ansi-without-scratch";
    case GU_STOP_GAX_NO_LOCALE: return "advapi-ansi-without-code-page";
    case GU_STOP_GAX_OPTIONS: return "advapi-key-options-unsupported";
    case GU_STOP_GAX_SECURITY: return "advapi-security-attributes-unsupported";
    case GU_STOP_GAX_VALUE_TYPE: return "advapi-value-type-unsupported";
    case GU_STOP_GAX_TOO_LARGE: return "advapi-value-too-large-for-scratch";
    case GU_STOP_GAX_BAD_ARGS: return "advapi-argument-outside-model";
    case GU_STOP_GAX_PERSIST: return "advapi-overlay-persistence-failed";
    case GU_STOP_GAX_PROVIDER: return "advapi-crypto-provider-invalid";
    default: return "unknown-stop";
    }
}

/* --- guest string conversion ------------------------------------------- */
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
static int utf8_encode(uint32_t code, char *out, uint32_t capacity,
                       uint32_t *written) {
    if (code < 0x80) {
        if (capacity < 1) return GM_UNSUPPORTED;
        out[0] = (char)code;
        *written = 1;
    } else if (code < 0x800) {
        if (capacity < 2) return GM_UNSUPPORTED;
        out[0] = (char)(0xc0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3f));
        *written = 2;
    } else {
        if (capacity < 3) return GM_UNSUPPORTED;
        out[0] = (char)(0xe0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        *written = 3;
    }
    return GM_OK;
}
/* ANSI (CP1252) guest string -> UTF-8 host string. */
static int read_ansi(GuestAdvapiExtra *gax, uint32_t address, char *out,
                     uint32_t capacity) {
    if (!gax->locale || !gax->locale->installed) return GU_STOP_GAX_NO_LOCALE;
    uint32_t len = 0;
    for (uint32_t i = 0; i < 1024u; i++) {
        uint8_t byte;
        int rc = gm_read(gax->memory, address + i, &byte, 1);
        if (rc) return rc;
        if (!byte) break;
        uint32_t code = 0, written = 0;
        if (gl_decode_byte(gax->locale, byte, &code)) return GM_UNSUPPORTED;
        rc = utf8_encode(code, out + len, capacity - len, &written);
        if (rc) return GU_STOP_GAX_TOO_LARGE;
        len += written;
        if (i + 1 == 1024u) return GU_STOP_GAX_TOO_LARGE;
    }
    out[len] = 0;
    return GM_OK;
}
/* UTF-8 -> CP1252 bytes. */
static int encode_ansi(GuestAdvapiExtra *gax, const char *text, uint8_t *out,
                       uint32_t capacity, uint32_t *written) {
    if (!gax->locale || !gax->locale->installed) return GU_STOP_GAX_NO_LOCALE;
    uint32_t pos = 0, len = 0;
    while (text[pos]) {
        uint32_t code, used;
        int rc = utf8_decode(text + pos, &code, &used);
        if (rc) return rc;
        if (len >= capacity) return GU_STOP_GAX_TOO_LARGE;
        uint8_t byte = (code < 0x10000) ? gax->locale->encode[code] : 0;
        if (!byte) byte = '?';
        out[len++] = byte;
        pos += used;
    }
    *written = len;
    return GM_OK;
}
/* CP1252 bytes -> UTF-16LE bytes (a REG_SZ value is stored as UTF-16). */
static int ansi_to_wide(GuestAdvapiExtra *gax, const uint8_t *in, uint32_t size,
                        uint8_t *out, uint32_t capacity, uint32_t *written) {
    if (!gax->locale || !gax->locale->installed) return GU_STOP_GAX_NO_LOCALE;
    uint32_t units = 0;
    for (uint32_t i = 0; i < size; i++) {
        uint32_t code = 0;
        if (gl_decode_byte(gax->locale, in[i], &code)) return GM_UNSUPPORTED;
        if ((units + 1) * 2u > capacity) return GU_STOP_GAX_TOO_LARGE;
        out[units * 2u] = (uint8_t)code;
        out[units * 2u + 1] = (uint8_t)(code >> 8);
        units++;
    }
    *written = units * 2u;
    return GM_OK;
}

/* --- overlay key/value store ------------------------------------------- */
static GaxKey *overlay_find(GuestAdvapiExtra *gax, const char *path) {
    for (uint32_t i = 0; i < gax->key_count; i++)
        if (path_equal(gax->keys[i].path, path)) return &gax->keys[i];
    return NULL;
}
static GaxKey *overlay_ensure(GuestAdvapiExtra *gax, const char *path) {
    GaxKey *existing = overlay_find(gax, path);
    if (existing) return existing;
    if (gax->key_count == gax->key_capacity) {
        uint32_t capacity = gax->key_capacity ? gax->key_capacity * 2u : 8u;
        GaxKey *grown = (GaxKey *)realloc(gax->keys, capacity * sizeof(*grown));
        if (!grown) return NULL;
        memset(grown + gax->key_capacity, 0,
               (capacity - gax->key_capacity) * sizeof(*grown));
        gax->keys = grown;
        gax->key_capacity = capacity;
    }
    GaxKey *key = &gax->keys[gax->key_count];
    memset(key, 0, sizeof(*key));
    key->path = dup_string(path);
    if (!key->path) return NULL;
    gax->key_count++;
    return key;
}
static int overlay_remove(GuestAdvapiExtra *gax, GaxKey *key) {
    for (uint32_t v = 0; v < key->value_count; v++) {
        free(key->values[v].name);
        free(key->values[v].data);
    }
    free(key->values);
    free(key->path);
    uint32_t index = (uint32_t)(key - gax->keys);
    memmove(&gax->keys[index], &gax->keys[index + 1],
            (gax->key_count - index - 1) * sizeof(*gax->keys));
    gax->key_count--;
    return GM_OK;
}
static GaxValue *value_find(GaxKey *key, const char *name) {
    for (uint32_t i = 0; i < key->value_count; i++)
        if (path_equal(key->values[i].name, name)) return &key->values[i];
    return NULL;
}
static GaxValue *value_ensure(GaxKey *key, const char *name) {
    GaxValue *existing = value_find(key, name);
    if (existing) return existing;
    if (key->value_count == key->value_capacity) {
        uint32_t capacity = key->value_capacity ? key->value_capacity * 2u : 4u;
        GaxValue *grown = (GaxValue *)realloc(key->values,
                                              capacity * sizeof(*grown));
        if (!grown) return NULL;
        memset(grown + key->value_capacity, 0,
               (capacity - key->value_capacity) * sizeof(*grown));
        key->values = grown;
        key->value_capacity = capacity;
    }
    GaxValue *value = &key->values[key->value_count];
    memset(value, 0, sizeof(*value));
    value->name = dup_string(name);
    if (!value->name) return NULL;
    key->value_count++;
    return value;
}

/* --- JSON persistence --------------------------------------------------- */
static void js_put_string(FILE *file, const char *text) {
    fputc('"', file);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', file);
            fputc(*p, file);
        } else if (*p < 0x20) {
            fprintf(file, "\\u%04x", *p);
        } else {
            fputc(*p, file);
        }
    }
    fputc('"', file);
}
static int overlay_save(GuestAdvapiExtra *gax) {
    FILE *file = fopen(gax->overlay_path, "wb");
    if (!file) return GU_STOP_GAX_PERSIST;
    fputs("{\"schema\":1,\"keys\":[", file);
    for (uint32_t i = 0; i < gax->key_count; i++) {
        GaxKey *key = &gax->keys[i];
        if (i) fputc(',', file);
        fputs("{\"path\":", file);
        js_put_string(file, key->path);
        fputs(",\"values\":[", file);
        for (uint32_t v = 0; v < key->value_count; v++) {
            GaxValue *value = &key->values[v];
            if (v) fputc(',', file);
            fputs("{\"name\":", file);
            js_put_string(file, value->name);
            fprintf(file, ",\"type\":%u,\"data\":\"", value->type);
            for (uint32_t b = 0; b < value->size; b++)
                fprintf(file, "%02x", value->data[b]);
            fputs("\"}", file);
        }
        fputs("]}", file);
    }
    fputs("]}\n", file);
    if (fclose(file) != 0) return GU_STOP_GAX_PERSIST;
    gax->persist_writes++;
    return GM_OK;
}

typedef struct {
    const char *p, *end;
} Js;
static void js_ws(Js *js) {
    while (js->p < js->end && (*js->p == ' ' || *js->p == '\n' ||
                               *js->p == '\r' || *js->p == '\t'))
        js->p++;
}
static int js_expect(Js *js, char c) {
    js_ws(js);
    if (js->p >= js->end || *js->p != c) return GM_UNSUPPORTED;
    js->p++;
    return GM_OK;
}
static int js_string(Js *js, char *out, uint32_t capacity) {
    if (js_expect(js, '"')) return GM_UNSUPPORTED;
    uint32_t len = 0;
    while (js->p < js->end && *js->p != '"') {
        unsigned char c = (unsigned char)*js->p++;
        if (c == '\\') {
            if (js->p >= js->end) return GM_UNSUPPORTED;
            char escape = *js->p++;
            switch (escape) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                if (js->end - js->p < 4) return GM_UNSUPPORTED;
                char hex[5] = {js->p[0], js->p[1], js->p[2], js->p[3], 0};
                js->p += 4;
                uint32_t code = (uint32_t)strtoul(hex, NULL, 16);
                if (code >= 0x80) return GM_UNSUPPORTED; /* overlay is ASCII */
                c = (unsigned char)code;
                break;
            }
            default: return GM_UNSUPPORTED;
            }
        }
        if (len + 1 >= capacity) return GM_UNSUPPORTED;
        out[len++] = (char)c;
    }
    if (js->p >= js->end) return GM_UNSUPPORTED;
    js->p++; /* closing quote */
    out[len] = 0;
    return GM_OK;
}
static int js_uint(Js *js, uint32_t *out) {
    js_ws(js);
    uint32_t value = 0;
    int any = 0;
    while (js->p < js->end && *js->p >= '0' && *js->p <= '9') {
        value = value * 10u + (uint32_t)(*js->p - '0');
        js->p++;
        any = 1;
    }
    if (!any) return GM_UNSUPPORTED;
    *out = value;
    return GM_OK;
}
static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int overlay_load(GuestAdvapiExtra *gax) {
    FILE *file = fopen(gax->overlay_path, "rb");
    if (!file) return GM_OK; /* no overlay yet is not an error */
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return GM_OK; }
    long size = ftell(file);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(file); return GM_OK; }
    rewind(file);
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) { fclose(file); return GM_ALLOCATION; }
    size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[got] = 0;
    Js js = {text, text + got};
    char path[512], name[512], hex[2u * 8192u + 1u];
    int rc = GM_UNSUPPORTED;
    if (js_expect(&js, '{')) goto done;
    if (js_string(&js, name, sizeof(name)) || strcmp(name, "schema")) goto done;
    if (js_expect(&js, ':')) goto done;
    uint32_t schema = 0;
    if (js_uint(&js, &schema) || schema != 1) goto done;
    if (js_expect(&js, ',')) goto done;
    if (js_string(&js, name, sizeof(name)) || strcmp(name, "keys")) goto done;
    if (js_expect(&js, ':')) goto done;
    if (js_expect(&js, '[')) goto done;
    js_ws(&js);
    if (js.p < js.end && *js.p == ']') { js.p++; rc = GM_OK; goto done; }
    for (;;) {
        if (js_expect(&js, '{')) goto done;
        if (js_string(&js, name, sizeof(name)) || strcmp(name, "path")) goto done;
        if (js_expect(&js, ':')) goto done;
        if (js_string(&js, path, sizeof(path))) goto done;
        if (js_expect(&js, ',')) goto done;
        if (js_string(&js, name, sizeof(name)) || strcmp(name, "values")) goto done;
        if (js_expect(&js, ':')) goto done;
        GaxKey *key = overlay_ensure(gax, path);
        if (!key) { rc = GM_ALLOCATION; goto done; }
        if (js_expect(&js, '[')) goto done;
        js_ws(&js);
        if (js.p < js.end && *js.p == ']') {
            js.p++;
        } else {
            for (;;) {
                if (js_expect(&js, '{')) goto done;
                if (js_string(&js, name, sizeof(name)) || strcmp(name, "name"))
                    goto done;
                if (js_expect(&js, ':')) goto done;
                char value_name[512];
                if (js_string(&js, value_name, sizeof(value_name))) goto done;
                if (js_expect(&js, ',')) goto done;
                if (js_string(&js, name, sizeof(name)) || strcmp(name, "type"))
                    goto done;
                if (js_expect(&js, ':')) goto done;
                uint32_t type = 0;
                if (js_uint(&js, &type)) goto done;
                if (js_expect(&js, ',')) goto done;
                if (js_string(&js, name, sizeof(name)) || strcmp(name, "data"))
                    goto done;
                if (js_expect(&js, ':')) goto done;
                if (js_string(&js, hex, sizeof(hex))) goto done;
                size_t hex_length = strlen(hex);
                if (hex_length % 2) goto done;
                uint32_t bytes = (uint32_t)(hex_length / 2);
                GaxValue *value = value_ensure(key, value_name);
                if (!value) { rc = GM_ALLOCATION; goto done; }
                free(value->data);
                value->data = bytes ? (uint8_t *)malloc(bytes) : NULL;
                if (bytes && !value->data) { rc = GM_ALLOCATION; goto done; }
                value->type = type;
                value->size = bytes;
                for (uint32_t b = 0; b < bytes; b++) {
                    int hi = hex_value(hex[b * 2]), lo = hex_value(hex[b * 2 + 1]);
                    if (hi < 0 || lo < 0) goto done;
                    value->data[b] = (uint8_t)((hi << 4) | lo);
                }
                if (js_expect(&js, '}')) goto done;
                js_ws(&js);
                if (js.p < js.end && *js.p == ',') { js.p++; continue; }
                if (js_expect(&js, ']')) goto done;
                break;
            }
        }
        if (js_expect(&js, '}')) goto done;
        js_ws(&js);
        if (js.p < js.end && *js.p == ',') { js.p++; continue; }
        if (js_expect(&js, ']')) goto done;
        break;
    }
    js_ws(&js);
    if (js_expect(&js, '}')) goto done;
    rc = GM_OK;
done:
    free(text);
    return rc;
}

/* --- path handling ------------------------------------------------------ */
static const char *root_name(uint32_t key, char *buffer, uint32_t capacity) {
    const char *name = NULL;
    switch (key) {
    case GWR_HKEY_CLASSES_ROOT: name = "HKCR"; break;
    case GWR_HKEY_CURRENT_USER: name = "HKCU"; break;
    case GWR_HKEY_LOCAL_MACHINE: name = "HKLM"; break;
    case GWR_HKEY_USERS: name = "HKU"; break;
    default: return NULL;
    }
    snprintf(buffer, capacity, "%s", name);
    return buffer;
}
/* Handle -> overlay path, or NULL when the handle is not ours. */
static const char *handle_path(GuestAdvapiExtra *gax, uint32_t handle,
                               char *buffer, uint32_t capacity) {
    char root[8];
    if (root_name(handle, root, sizeof(root))) {
        snprintf(buffer, capacity, "%s", root);
        return buffer;
    }
    GaxKeyRef *ref = (GaxKeyRef *)gh_lookup(handle, GH_KIND_OTHER, NULL);
    if (ref && ref->magic == GAX_KEY_MAGIC) {
        (void)gax;
        snprintf(buffer, capacity, "%s", ref->path);
        return buffer;
    }
    return NULL;
}
static int join_path(const char *parent, const char *child, char *out,
                     uint32_t capacity) {
    if (!child || !child[0]) {
        snprintf(out, capacity, "%s", parent);
        return GM_OK;
    }
    /* A leading backslash means "relative to the root of the hive". */
    if (child[0] == '\\') {
        const char *root = strchr(parent, '\\');
        uint32_t root_length = root ? (uint32_t)(root - parent) : (uint32_t)strlen(parent);
        int n = snprintf(out, capacity, "%.*s%s", (int)root_length, parent, child);
        return (n < 0 || (uint32_t)n >= capacity) ? GM_UNSUPPORTED : GM_OK;
    }
    int n = snprintf(out, capacity, "%s\\%s", parent, child);
    return (n < 0 || (uint32_t)n >= capacity) ? GM_UNSUPPORTED : GM_OK;
}
/* Splits "HKCU\Software\X" into the predefined root and the subkey. */
static int split_path(const char *path, uint32_t *root, const char **subkey) {
    if (!strncmp(path, "HKCR", 4)) { *root = GWR_HKEY_CLASSES_ROOT; path += 4; }
    else if (!strncmp(path, "HKCU", 4)) { *root = GWR_HKEY_CURRENT_USER; path += 4; }
    else if (!strncmp(path, "HKLM", 4)) { *root = GWR_HKEY_LOCAL_MACHINE; path += 4; }
    else if (!strncmp(path, "HKU", 3)) { *root = GWR_HKEY_USERS; path += 3; }
    else return GM_UNSUPPORTED;
    while (*path == '\\') path++;
    *subkey = path;
    return GM_OK;
}
static int scratch_ready(GuestAdvapiExtra *gax) {
    return gax->scratch && gax->scratch_size >= GAX_SCRATCH_MIN;
}
/* Writes a UTF-16LE string into the guest scratch name slot: the read-only
 * projection exposes the W entry points (RegOpenKeyExW/RegQueryValueExW), so
 * every name handed to it has to be wide. */
static int scratch_name(GuestAdvapiExtra *gax, const char *text) {
    uint32_t pos = 0, units = 0;
    while (text[pos]) {
        uint32_t code, used;
        int rc = utf8_decode(text + pos, &code, &used);
        if (rc) return rc;
        if (code > 0xffff) return GM_UNSUPPORTED;
        if ((units + 1u) * 2u > GAX_SCRATCH_NAME_SIZE)
            return GU_STOP_GAX_TOO_LARGE;
        uint8_t bytes[2] = {(uint8_t)code, (uint8_t)(code >> 8)};
        rc = gm_write(gax->memory,
                      gax->scratch + GAX_SCRATCH_NAME + units * 2u, bytes, 2);
        if (rc) return rc;
        units++;
        pos += used;
    }
    uint8_t nul[2] = {0, 0};
    return gm_write(gax->memory,
                    gax->scratch + GAX_SCRATCH_NAME + units * 2u, nul, 2);
}
/* Opens the read-only projection for a full overlay path. */
static int projection_open(GuestAdvapiExtra *gax, const char *path,
                           uint32_t *handle, uint32_t *status) {
    uint32_t root = 0;
    const char *subkey = NULL;
    int rc = split_path(path, &root, &subkey);
    if (rc) return rc;
    if (!gax->registry) { *status = GAX_ERROR_FILE_NOT_FOUND; return GM_OK; }
    rc = scratch_name(gax, subkey);
    if (rc) return rc;
    uint32_t slot = gax->scratch + GAX_SCRATCH_TYPE + 4u;
    rc = gwr_open(gax->registry, gax->memory, gax->teb, root,
                  gax->scratch + GAX_SCRATCH_NAME, 0, 0x20019u, slot, status);
    if (rc) return rc;
    uint8_t bytes[4];
    rc = gm_read(gax->memory, slot, bytes, 4);
    if (rc) return rc;
    *handle = get32(bytes);
    return GM_OK;
}
static int key_handle(GuestAdvapiExtra *gax, const char *path, uint32_t *handle) {
    (void)gax;
    GaxKeyRef *ref = (GaxKeyRef *)calloc(1, sizeof(*ref));
    if (!ref) return GM_ALLOCATION;
    ref->path = dup_string(path);
    if (!ref->path) { free(ref); return GM_ALLOCATION; }
    ref->magic = GAX_KEY_MAGIC;
    uint32_t issued = gh_insert(GH_KIND_OTHER, ref, free);
    if (!issued) { free(ref->path); free(ref); return GM_ALLOCATION; }
    *handle = issued;
    return GM_OK;
}

/* RegCloseKey for a key handle this overlay issued. 1 = it was ours (closed,
 * *result = ERROR_SUCCESS), 0 = not an overlay handle (the caller asks the
 * read-only projection). */
int gax_reg_close_key_if_overlay(GuestAdvapiExtra *gax, uint32_t handle, uint32_t *result) {
    if (!gax || !result) return 0;
    GaxKeyRef *ref = (GaxKeyRef *)gh_lookup(handle, GH_KIND_OTHER, NULL);
    if (!ref || ref->magic != GAX_KEY_MAGIC) return 0;
    if (!gh_close(handle)) return 0;
    (void)last_error(gax, GAX_ERROR_SUCCESS);
    *result = GAX_ERROR_SUCCESS;
    return 1;
}

/* --- advapi32 ---------------------------------------------------------- */
int gax_reg_open_key_ex_a(GuestAdvapiExtra *gax, const uint32_t *args,
                          uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    if (args[2]) return stop(gax, GU_STOP_GAX_OPTIONS,
                             "RegOpenKeyExA: ulOptions must be 0");
    char parent[512], path[1024], subkey[512];
    const char *parent_path = handle_path(gax, args[0], parent, sizeof(parent));
    if (!parent_path) {
        int rc = last_error(gax, GAX_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = GAX_ERROR_INVALID_HANDLE;
        return GM_OK;
    }
    if (args[1]) {
        int rc = read_ansi(gax, args[1], subkey, sizeof(subkey));
        if (rc == GU_STOP_GAX_TOO_LARGE) {
            *result = GAX_ERROR_INVALID_NAME;
            return GM_OK;
        }
        if (rc) return rc;
    } else {
        subkey[0] = 0;
    }
    if (join_path(parent_path, subkey, path, sizeof(path))) {
        *result = GAX_ERROR_INVALID_NAME;
        return GM_OK;
    }
    if (!scratch_ready(gax)) return stop(gax, GU_STOP_GAX_NO_SCRATCH,
                                         "RegOpenKeyExA: no guest scratch");
    if (args[3] & GWR_WRITE_ACCESS) {
        /* Writable open: the overlay must own the key so writes can land. */
        if (!overlay_find(gax, path)) {
            uint32_t projection = 0, status = 0;
            int rc = projection_open(gax, path, &projection, &status);
            if (rc) return rc;
            if (status != GAX_ERROR_SUCCESS) {
                *result = status;
                return GM_OK;
            }
            (void)gwr_close(gax->registry, gax->memory, gax->teb, projection,
                            &status);
            if (!overlay_ensure(gax, path)) return GM_ALLOCATION;
        }
        uint32_t handle = 0;
        int rc = key_handle(gax, path, &handle);
        if (rc) return rc;
        rc = write_u32(gax, args[4], handle);
        if (rc) return rc;
        *result = GAX_ERROR_SUCCESS;
        return GM_OK;
    }
    /* Read-only open: the projection first, then the overlay shadow. */
    int rc = scratch_name(gax, subkey);
    if (rc == GU_STOP_GAX_TOO_LARGE) { *result = GAX_ERROR_INVALID_NAME; return GM_OK; }
    if (rc) return rc;
    uint32_t status = 0;
    rc = gwr_open(gax->registry, gax->memory, gax->teb, args[0],
                  gax->scratch + GAX_SCRATCH_NAME, 0, args[3], args[4],
                  &status);
    if (rc) return rc;
    if (status == GAX_ERROR_SUCCESS) { *result = status; return GM_OK; }
    if (overlay_find(gax, path)) {
        uint32_t handle = 0;
        rc = key_handle(gax, path, &handle);
        if (rc) return rc;
        rc = write_u32(gax, args[4], handle);
        if (rc) return rc;
        *result = GAX_ERROR_SUCCESS;
        return GM_OK;
    }
    *result = status;
    return GM_OK;
}
/* Copies a value out of the overlay. */
static int overlay_query(GuestAdvapiExtra *gax, GaxValue *value, uint32_t type,
                         uint32_t data, uint32_t size, uint32_t *result) {
    uint8_t out[GAX_SCRATCH_DATA_SIZE];
    uint32_t length = value->size;
    uint32_t out_type = value->type;
    const uint8_t *bytes = value->data;
    if (value->type == GAX_REG_SZ || value->type == GAX_REG_EXPAND_SZ ||
        value->type == GAX_REG_MULTI_SZ) {
        /* UTF-16LE -> CP1252 for the ANSI entry point. */
        uint32_t units = value->size / 2u, len = 0;
        for (uint32_t i = 0; i < units; i++) {
            uint32_t code = (uint32_t)value->data[i * 2u] |
                            ((uint32_t)value->data[i * 2u + 1] << 8);
            uint8_t byte = (code < 0x10000) ? gax->locale->encode[code] : 0;
            if (!byte && code) byte = '?';
            if (len + 1 >= sizeof(out)) return GU_STOP_GAX_TOO_LARGE;
            out[len++] = byte;
            if (!code) break;
        }
        length = len;
        bytes = out;
    }
    if (type) {
        int rc = write_u32(gax, type, out_type);
        if (rc) return rc;
    }
    if (!size) { *result = GAX_ERROR_SUCCESS; return GM_OK; }
    uint8_t capacity_bytes[4];
    int rc = gm_read(gax->memory, size, capacity_bytes, 4);
    if (rc) return rc;
    uint32_t capacity = get32(capacity_bytes);
    if (!data) {
        rc = write_u32(gax, size, length);
        if (rc) return rc;
        *result = GAX_ERROR_SUCCESS;
        return GM_OK;
    }
    if (capacity < length) {
        rc = write_u32(gax, size, length);
        if (rc) return rc;
        *result = GAX_ERROR_MORE_DATA;
        return GM_OK;
    }
    if (length) {
        rc = gm_write(gax->memory, data, bytes, length);
        if (rc) return rc;
    }
    rc = write_u32(gax, size, length);
    if (rc) return rc;
    *result = GAX_ERROR_SUCCESS;
    return GM_OK;
}
/* Queries the unchanged read-only projection through the W entry point and
 * converts the UTF-16 value back to CP1252 for the ANSI caller. */
static int projection_query(GuestAdvapiExtra *gax, uint32_t handle,
                            const char *name, uint32_t type, uint32_t data,
                            uint32_t size, uint32_t *result) {
    if (!scratch_ready(gax)) return stop(gax, GU_STOP_GAX_NO_SCRATCH,
                                         "RegQueryValueExA: no guest scratch");
    uint32_t status = 0;
    int rc = scratch_name(gax, name);
    if (rc == GU_STOP_GAX_TOO_LARGE) { *result = GAX_ERROR_INVALID_NAME; return GM_OK; }
    if (rc) return rc;
    uint32_t capacity = GAX_SCRATCH_DATA_SIZE;
    rc = write_u32(gax, gax->scratch + GAX_SCRATCH_TYPE + 4u, capacity);
    if (rc) return rc;
    rc = gwr_query(gax->registry, gax->memory, gax->teb, handle,
                   gax->scratch + GAX_SCRATCH_NAME, 0,
                   gax->scratch + GAX_SCRATCH_TYPE,
                   gax->scratch + GAX_SCRATCH_DATA,
                   gax->scratch + GAX_SCRATCH_TYPE + 4u, &status);
    if (rc) return rc;
    if (status != GAX_ERROR_SUCCESS) { *result = status; return GM_OK; }
    uint8_t type_bytes[4], size_bytes[4];
    rc = gm_read(gax->memory, gax->scratch + GAX_SCRATCH_TYPE, type_bytes, 4);
    if (rc) return rc;
    rc = gm_read(gax->memory, gax->scratch + GAX_SCRATCH_TYPE + 4u, size_bytes, 4);
    if (rc) return rc;
    uint32_t value_type = get32(type_bytes);
    uint32_t value_size = get32(size_bytes);
    if (value_size > GAX_SCRATCH_DATA_SIZE) return GU_STOP_GAX_TOO_LARGE;
    uint8_t raw[GAX_SCRATCH_DATA_SIZE];
    if (value_size) {
        rc = gm_read(gax->memory, gax->scratch + GAX_SCRATCH_DATA, raw, value_size);
        if (rc) return rc;
    }
    uint8_t converted[GAX_SCRATCH_DATA_SIZE];
    uint32_t length = value_size;
    const uint8_t *bytes = raw;
    if (value_type == GAX_REG_SZ || value_type == GAX_REG_EXPAND_SZ ||
        value_type == GAX_REG_MULTI_SZ) {
        uint32_t units = value_size / 2u, len = 0;
        for (uint32_t i = 0; i < units; i++) {
            uint32_t code = (uint32_t)raw[i * 2u] | ((uint32_t)raw[i * 2u + 1] << 8);
            uint8_t byte = (code < 0x10000) ? gax->locale->encode[code] : 0;
            if (!byte && code) byte = '?';
            if (len + 1 >= sizeof(converted)) return GU_STOP_GAX_TOO_LARGE;
            converted[len++] = byte;
            if (!code) break;
        }
        length = len;
        bytes = converted;
    }
    if (type) {
        rc = write_u32(gax, type, value_type);
        if (rc) return rc;
    }
    if (!size) { *result = GAX_ERROR_SUCCESS; return GM_OK; }
    uint8_t capacity_bytes[4];
    rc = gm_read(gax->memory, size, capacity_bytes, 4);
    if (rc) return rc;
    uint32_t user_capacity = get32(capacity_bytes);
    if (!data) {
        rc = write_u32(gax, size, length);
        if (rc) return rc;
        *result = GAX_ERROR_SUCCESS;
        return GM_OK;
    }
    if (user_capacity < length) {
        rc = write_u32(gax, size, length);
        if (rc) return rc;
        *result = GAX_ERROR_MORE_DATA;
        return GM_OK;
    }
    if (length) {
        rc = gm_write(gax->memory, data, bytes, length);
        if (rc) return rc;
    }
    rc = write_u32(gax, size, length);
    if (rc) return rc;
    *result = GAX_ERROR_SUCCESS;
    return GM_OK;
}

int gax_reg_query_value_ex_a(GuestAdvapiExtra *gax, const uint32_t *args,
                             uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    if (args[2]) return stop(gax, GU_STOP_GAX_BAD_ARGS,
                             "RegQueryValueExA: lpReserved must be NULL");
    char parent[512], name[512];
    const char *path = handle_path(gax, args[0], parent, sizeof(parent));
    if (args[1]) {
        int rc = read_ansi(gax, args[1], name, sizeof(name));
        if (rc == GU_STOP_GAX_TOO_LARGE) { *result = GAX_ERROR_INVALID_NAME; return GM_OK; }
        if (rc) return rc;
    } else {
        name[0] = 0;
    }
    if (!path) {
        /* Not an overlay key: a handle issued by the read-only projection is
         * queried directly, never re-opened by path. */
        return projection_query(gax, args[0], name, args[3], args[4], args[5],
                                result);
    }
    GaxKey *key = overlay_find(gax, path);
    if (key) {
        GaxValue *value = value_find(key, name);
        if (value)
            return overlay_query(gax, value, args[3], args[4], args[5], result);
    }
    /* Fall through to the unchanged read-only projection. */
    uint32_t projection = 0, status = 0;
    int rc = projection_open(gax, path, &projection, &status);
    if (rc) return rc;
    if (status != GAX_ERROR_SUCCESS) { *result = status; return GM_OK; }
    rc = projection_query(gax, projection, name, args[3], args[4], args[5],
                          result);
    uint32_t ignored = 0;
    (void)gwr_close(gax->registry, gax->memory, gax->teb, projection, &ignored);
    return rc;
}
int gax_reg_create_key_ex_a(GuestAdvapiExtra *gax, const uint32_t *args,
                            uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    if (args[2]) return stop(gax, GU_STOP_GAX_BAD_ARGS,
                             "RegCreateKeyExA: Reserved must be 0");
    if (args[3]) return stop(gax, GU_STOP_GAX_SECURITY,
                             "RegCreateKeyExA: lpClass is not modelled");
    if (args[4] != GAX_REG_OPTION_NON_VOLATILE)
        return stop(gax, GU_STOP_GAX_OPTIONS,
                    "RegCreateKeyExA: only REG_OPTION_NON_VOLATILE is modelled");
    if (args[6]) return stop(gax, GU_STOP_GAX_SECURITY,
                             "RegCreateKeyExA: lpSecurityAttributes must be NULL");
    char parent[512], path[1024], subkey[512];
    const char *parent_path = handle_path(gax, args[0], parent, sizeof(parent));
    if (!parent_path) {
        int rc = last_error(gax, GAX_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = GAX_ERROR_INVALID_HANDLE;
        return GM_OK;
    }
    if (args[1]) {
        int rc = read_ansi(gax, args[1], subkey, sizeof(subkey));
        if (rc == GU_STOP_GAX_TOO_LARGE) { *result = GAX_ERROR_INVALID_NAME; return GM_OK; }
        if (rc) return rc;
    } else {
        subkey[0] = 0;
    }
    if (join_path(parent_path, subkey, path, sizeof(path))) {
        *result = GAX_ERROR_INVALID_NAME;
        return GM_OK;
    }
    /* RegCreateKeyEx creates every missing intermediate key as well. */
    char prefix[1024];
    for (uint32_t i = 0; path[i]; i++) {
        if (path[i] != '\\') continue;
        memcpy(prefix, path, i);
        prefix[i] = 0;
        if (strchr(prefix, '\\') && !overlay_ensure(gax, prefix))
            return GM_ALLOCATION;
    }
    int existed = overlay_find(gax, path) != NULL;
    if (!existed && gax->registry) {
        /* A key that already exists in the projection is "opened", not new. */
        uint32_t projection = 0, status = 0;
        int rc = projection_open(gax, path, &projection, &status);
        if (rc) return rc;
        if (status == GAX_ERROR_SUCCESS) {
            uint32_t ignored = 0;
            (void)gwr_close(gax->registry, gax->memory, gax->teb, projection,
                            &ignored);
            existed = 1;
        }
    }
    if (!overlay_ensure(gax, path)) return GM_ALLOCATION;
    uint32_t handle = 0;
    int rc = key_handle(gax, path, &handle);
    if (rc) return rc;
    rc = write_u32(gax, args[7], handle);
    if (rc) return rc;
    if (args[8]) {
        rc = write_u32(gax, args[8],
                       existed ? GAX_REG_OPENED_EXISTING_KEY
                               : GAX_REG_CREATED_NEW_KEY);
        if (rc) return rc;
    }
    rc = overlay_save(gax);
    if (rc) return rc;
    *result = GAX_ERROR_SUCCESS;
    return GM_OK;
}
int gax_reg_set_value_ex_a(GuestAdvapiExtra *gax, const uint32_t *args,
                           uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    if (args[2]) return stop(gax, GU_STOP_GAX_BAD_ARGS,
                             "RegSetValueExA: Reserved must be 0");
    char parent[512], name[512];
    const char *path = handle_path(gax, args[0], parent, sizeof(parent));
    if (!path) {
        /* A handle of the read-only projection is not writable. */
        int rc = last_error(gax, GAX_ERROR_ACCESS_DENIED);
        if (rc) return rc;
        *result = GAX_ERROR_ACCESS_DENIED;
        return GM_OK;
    }
    GaxKey *key = overlay_find(gax, path);
    if (!key) {
        /* A projection-only key cannot be written: its bytes are evidence. */
        int rc = last_error(gax, GAX_ERROR_ACCESS_DENIED);
        if (rc) return rc;
        *result = GAX_ERROR_ACCESS_DENIED;
        return GM_OK;
    }
    if (args[1]) {
        int rc = read_ansi(gax, args[1], name, sizeof(name));
        if (rc == GU_STOP_GAX_TOO_LARGE) { *result = GAX_ERROR_INVALID_NAME; return GM_OK; }
        if (rc) return rc;
    } else {
        name[0] = 0;
    }
    uint32_t type = args[3], size = args[5];
    uint8_t raw[GAX_SCRATCH_DATA_SIZE];
    if (size > sizeof(raw)) return stop(gax, GU_STOP_GAX_TOO_LARGE,
                                        "RegSetValueExA: value larger than the "
                                        "modelled scratch");
    if (size && !args[4]) return stop(gax, GU_STOP_GAX_BAD_ARGS,
                                      "RegSetValueExA: NULL data with a size");
    if (size) {
        int rc = gm_read(gax->memory, args[4], raw, size);
        if (rc) return rc;
    }
    uint8_t wide[2u * GAX_SCRATCH_DATA_SIZE];
    const uint8_t *stored = raw;
    uint32_t stored_size = size;
    uint32_t written = 0;
    switch (type) {
    case GAX_REG_SZ:
    case GAX_REG_EXPAND_SZ:
    case GAX_REG_MULTI_SZ: {
        int rc = ansi_to_wide(gax, raw, size, wide, sizeof(wide), &written);
        if (rc) return rc;
        stored = wide;
        stored_size = written;
        break;
    }
    case GAX_REG_BINARY:
    case GAX_REG_DWORD:
    case GAX_REG_QWORD:
    case GAX_REG_NONE:
        break;
    default:
        return stop(gax, GU_STOP_GAX_VALUE_TYPE,
                    "RegSetValueExA: value type not modelled");
    }
    GaxValue *value = value_ensure(key, name);
    if (!value) return GM_ALLOCATION;
    uint8_t *copy = stored_size ? (uint8_t *)malloc(stored_size) : NULL;
    if (stored_size && !copy) return GM_ALLOCATION;
    if (stored_size) memcpy(copy, stored, stored_size);
    free(value->data);
    value->data = copy;
    value->size = stored_size;
    value->type = type;
    int rc = overlay_save(gax);
    if (rc) return rc;
    *result = GAX_ERROR_SUCCESS;
    return GM_OK;
}
int gax_reg_delete_key_a(GuestAdvapiExtra *gax, const uint32_t *args,
                         uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    char parent[512], path[1024], subkey[512];
    const char *parent_path = handle_path(gax, args[0], parent, sizeof(parent));
    if (!parent_path) {
        int rc = last_error(gax, GAX_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = GAX_ERROR_INVALID_HANDLE;
        return GM_OK;
    }
    if (!args[1]) return stop(gax, GU_STOP_GAX_BAD_ARGS,
                              "RegDeleteKeyA: lpSubKey must not be NULL");
    int rc = read_ansi(gax, args[1], subkey, sizeof(subkey));
    if (rc == GU_STOP_GAX_TOO_LARGE) { *result = GAX_ERROR_INVALID_NAME; return GM_OK; }
    if (rc) return rc;
    if (join_path(parent_path, subkey, path, sizeof(path))) {
        *result = GAX_ERROR_INVALID_NAME;
        return GM_OK;
    }
    GaxKey *key = overlay_find(gax, path);
    if (!key) {
        uint32_t projection = 0, status = GAX_ERROR_FILE_NOT_FOUND;
        if (gax->registry) {
            rc = projection_open(gax, path, &projection, &status);
            if (rc) return rc;
            if (status == GAX_ERROR_SUCCESS) {
                uint32_t ignored = 0;
                (void)gwr_close(gax->registry, gax->memory, gax->teb,
                                projection, &ignored);
                status = GAX_ERROR_ACCESS_DENIED;
            }
        }
        *result = status;
        return GM_OK;
    }
    /* Windows refuses to delete a key that still has subkeys. */
    size_t prefix = strlen(path);
    for (uint32_t i = 0; i < gax->key_count; i++) {
        const char *other = gax->keys[i].path;
        if (other == key->path) continue;
        if (!strncmp(other, path, prefix) && other[prefix] == '\\') {
            *result = GAX_ERROR_ACCESS_DENIED;
            return GM_OK;
        }
    }
    rc = overlay_remove(gax, key);
    if (rc) return rc;
    rc = overlay_save(gax);
    if (rc) return rc;
    *result = GAX_ERROR_SUCCESS;
    return GM_OK;
}
int gax_reg_enum_key_ex_a(GuestAdvapiExtra *gax, const uint32_t *args,
                          uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    char parent[512], path[1024];
    const char *parent_path = handle_path(gax, args[0], parent, sizeof(parent));
    if (!parent_path) {
        int rc = last_error(gax, GAX_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = GAX_ERROR_INVALID_HANDLE;
        return GM_OK;
    }
    snprintf(path, sizeof(path), "%s", parent_path);
    size_t prefix = strlen(path);
    uint32_t index = args[1], seen = 0;
    const char *found = NULL;
    for (uint32_t i = 0; i < gax->key_count; i++) {
        const char *other = gax->keys[i].path;
        if (strncmp(other, path, prefix) != 0 || other[prefix] != '\\') continue;
        const char *child = other + prefix + 1;
        if (strchr(child, '\\')) continue; /* only direct children */
        if (seen++ == index) { found = child; break; }
    }
    if (!found) {
        *result = GAX_ERROR_NO_MORE_ITEMS;
        return GM_OK;
    }
    if (args[3]) {
        uint32_t capacity = 0;
        uint8_t bytes[4];
        int rc = gm_read(gax->memory, args[3], bytes, 4);
        if (rc) return rc;
        capacity = get32(bytes);
        uint8_t ansi[512];
        uint32_t written = 0;
        rc = encode_ansi(gax, found, ansi, sizeof(ansi), &written);
        if (rc) return rc;
        if (capacity < written + 1u) {
            rc = write_u32(gax, args[3], written + 1u);
            if (rc) return rc;
            *result = GAX_ERROR_MORE_DATA;
            return GM_OK;
        }
        if (!args[2]) return stop(gax, GU_STOP_GAX_BAD_ARGS,
                                  "RegEnumKeyExA: NULL name with capacity");
        ansi[written] = 0;
        rc = gm_write(gax->memory, args[2], ansi, written + 1u);
        if (rc) return rc;
        rc = write_u32(gax, args[3], written);
        if (rc) return rc;
    }
    if (args[5]) {
        int rc = write_u32(gax, args[5], 0); /* no class */
        if (rc) return rc;
    }
    if (args[4]) {
        int rc = write_u32(gax, args[4], 0); /* lpReserved */
        if (rc) return rc;
    }
    if (args[7]) {
        uint8_t zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int rc = gm_write(gax->memory, args[7], zero, 8);
        if (rc) return rc;
    }
    *result = GAX_ERROR_SUCCESS;
    return GM_OK;
}
int gax_reg_query_info_key_w(GuestAdvapiExtra *gax, const uint32_t *args,
                             uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    char parent[512], path[1024];
    const char *parent_path = handle_path(gax, args[0], parent, sizeof(parent));
    if (!parent_path) {
        int rc = last_error(gax, GAX_ERROR_INVALID_HANDLE);
        if (rc) return rc;
        *result = GAX_ERROR_INVALID_HANDLE;
        return GM_OK;
    }
    snprintf(path, sizeof(path), "%s", parent_path);
    size_t prefix = strlen(path);
    GaxKey *key = overlay_find(gax, path);
    uint32_t subkeys = 0, max_subkey = 0, values = 0, max_name = 0, max_data = 0;
    for (uint32_t i = 0; i < gax->key_count; i++) {
        const char *other = gax->keys[i].path;
        if (strncmp(other, path, prefix) != 0 || other[prefix] != '\\') continue;
        const char *child = other + prefix + 1;
        if (strchr(child, '\\')) continue;
        subkeys++;
        uint32_t length = (uint32_t)strlen(child);
        if (length > max_subkey) max_subkey = length;
    }
    if (key) {
        values = key->value_count;
        for (uint32_t v = 0; v < key->value_count; v++) {
            uint32_t length = (uint32_t)strlen(key->values[v].name);
            if (length > max_name) max_name = length;
            if (key->values[v].size > max_data) max_data = key->values[v].size;
        }
    }
    if (args[2]) {
        int rc = write_u32(gax, args[2], 0); /* no class */
        if (rc) return rc;
    }
    if (args[3]) {
        int rc = write_u32(gax, args[3], 0); /* lpReserved */
        if (rc) return rc;
    }
    if (args[4]) { int rc = write_u32(gax, args[4], subkeys); if (rc) return rc; }
    if (args[5]) { int rc = write_u32(gax, args[5], max_subkey); if (rc) return rc; }
    if (args[6]) { int rc = write_u32(gax, args[6], 0); if (rc) return rc; }
    if (args[7]) { int rc = write_u32(gax, args[7], values); if (rc) return rc; }
    if (args[8]) { int rc = write_u32(gax, args[8], max_name); if (rc) return rc; }
    if (args[9]) { int rc = write_u32(gax, args[9], max_data); if (rc) return rc; }
    if (args[10]) {
        int rc = write_u32(gax, args[10], 0); /* no security descriptor */
        if (rc) return rc;
    }
    if (args[11]) {
        uint8_t zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int rc = gm_write(gax->memory, args[11], zero, 8);
        if (rc) return rc;
    }
    *result = GAX_ERROR_SUCCESS;
    return GM_OK;
}

/* --- crypto ------------------------------------------------------------ */
static int provider_type_known(uint32_t type) {
    switch (type) {
    case GAX_PROV_RSA_FULL:
    case GAX_PROV_RSA_SIG:
    case GAX_PROV_DSS:
    case GAX_PROV_RSA_SCHANNEL:
    case GAX_PROV_DH_SCHANNEL:
    case GAX_PROV_RSA_AES:
        return 1;
    default:
        return 0;
    }
}
int gax_crypt_acquire_context_a(GuestAdvapiExtra *gax, const uint32_t *args,
                                uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    uint32_t type = args[3], flags = args[4];
    if (!provider_type_known(type)) {
        *result = 0;
        int rc = last_error(gax, GAX_NTE_BAD_PROV_TYPE);
        if (rc) return rc;
        return GM_OK;
    }
    if (flags & ~(GAX_CRYPT_VERIFYCONTEXT | GAX_CRYPT_NEWKEYSET |
                  GAX_CRYPT_DELETEKEYSET | GAX_CRYPT_MACHINE_KEYSET |
                  GAX_CRYPT_SILENT)) {
        *result = 0;
        int rc = last_error(gax, GAX_NTE_BAD_FLAGS);
        if (rc) return rc;
        return GM_OK;
    }
    if (!args[0]) return stop(gax, GU_STOP_GAX_BAD_ARGS,
                              "CryptAcquireContextA: phProv must not be NULL");
    if (flags & (GAX_CRYPT_NEWKEYSET | GAX_CRYPT_DELETEKEYSET)) {
        /* No key container store exists in this runtime. */
        int rc = write_u32(gax, args[0], 0);
        if (rc) return rc;
        rc = last_error(gax, GAX_NTE_BAD_KEYSET);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    if (!(flags & GAX_CRYPT_VERIFYCONTEXT)) {
        /* A named key container cannot be served without a key store. */
        int rc = write_u32(gax, args[0], 0);
        if (rc) return rc;
        rc = last_error(gax, GAX_NTE_BAD_KEYSET);
        if (rc) return rc;
        *result = 0;
        return GM_OK;
    }
    GaxProvider *provider = (GaxProvider *)calloc(1, sizeof(*provider));
    if (!provider) return GM_ALLOCATION;
    provider->provider_type = type;
    provider->magic = GAX_PROV_MAGIC;
    uint32_t handle = gh_insert(GH_KIND_OTHER, provider, free);
    if (!handle) { free(provider); return GM_ALLOCATION; }
    int rc = write_u32(gax, args[0], handle);
    if (rc) return rc;
    gax->provider_count++;
    *result = 1;
    return GM_OK;
}
static GaxProvider *provider_lookup(uint32_t handle) {
    GaxProvider *provider = (GaxProvider *)gh_lookup(handle, GH_KIND_OTHER, NULL);
    if (!provider || provider->magic != GAX_PROV_MAGIC) return NULL;
    return provider;
}
int gax_crypt_gen_random(GuestAdvapiExtra *gax, const uint32_t *args,
                         uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    if (!provider_lookup(args[0]))
        return stop(gax, GU_STOP_GAX_PROVIDER,
                    "CryptGenRandom: handle is not a provider issued here");
    uint32_t length = args[1];
    if (!length) { *result = 1; return GM_OK; }
    if (!args[2]) return stop(gax, GU_STOP_GAX_BAD_ARGS,
                              "CryptGenRandom: pbBuffer must not be NULL");
    int rc = gm_check(gax->memory, args[2], length, GM_WRITE);
    if (rc) return rc;
    uint8_t chunk[256];
    uint32_t done = 0;
    while (done < length) {
        uint32_t step = length - done;
        if (step > sizeof(chunk)) step = sizeof(chunk);
        ssize_t got = getrandom(chunk, step, 0);
        if (got <= 0) {
            FILE *urandom = fopen("/dev/urandom", "rb");
            if (!urandom) return stop(gax, GU_STOP_GAX_PROVIDER,
                                      "CryptGenRandom: no entropy source");
            size_t read_bytes = fread(chunk, 1, step, urandom);
            fclose(urandom);
            if (read_bytes != step)
                return stop(gax, GU_STOP_GAX_PROVIDER,
                            "CryptGenRandom: /dev/urandom short read");
        }
        rc = gm_write(gax->memory, args[2] + done, chunk, step);
        if (rc) return rc;
        done += step;
        gax->random_bytes += step;
    }
    *result = 1;
    return GM_OK;
}
int gax_crypt_release_context(GuestAdvapiExtra *gax, const uint32_t *args,
                              uint32_t *result) {
    if (!gax || !args || !result) return GU_STOP_GAX_BAD_ARGS;
    if (!provider_lookup(args[0]))
        return stop(gax, GU_STOP_GAX_PROVIDER,
                    "CryptReleaseContext: handle is not a provider issued here");
    if (!gh_close(args[0])) return GU_STOP_GAX_PROVIDER;
    if (gax->provider_count) gax->provider_count--;
    *result = 1;
    return GM_OK;
}

/* --- registration list -------------------------------------------------- */
#define GAX_ARGS(n) ((n) * 4u)
const GuestAdvapiExtraExport advapi32_extra_exports[] = {
    { "advapi32.dll", "RegOpenKeyExA", GAX_ARGS(5), gax_reg_open_key_ex_a },
    { "advapi32.dll", "RegQueryValueExA", GAX_ARGS(6),
      gax_reg_query_value_ex_a },
    { "advapi32.dll", "RegCreateKeyExA", GAX_ARGS(9),
      gax_reg_create_key_ex_a },
    { "advapi32.dll", "RegSetValueExA", GAX_ARGS(6), gax_reg_set_value_ex_a },
    { "advapi32.dll", "RegDeleteKeyA", GAX_ARGS(2), gax_reg_delete_key_a },
    { "advapi32.dll", "RegEnumKeyExA", GAX_ARGS(8), gax_reg_enum_key_ex_a },
    { "advapi32.dll", "RegQueryInfoKeyW", GAX_ARGS(12),
      gax_reg_query_info_key_w },
    { "advapi32.dll", "CryptAcquireContextA", GAX_ARGS(5),
      gax_crypt_acquire_context_a },
    { "advapi32.dll", "CryptGenRandom", GAX_ARGS(3), gax_crypt_gen_random },
    { "advapi32.dll", "CryptReleaseContext", GAX_ARGS(2),
      gax_crypt_release_context }
};
const uint32_t advapi32_extra_export_count =
    (uint32_t)(sizeof(advapi32_extra_exports) /
               sizeof(advapi32_extra_exports[0]));
