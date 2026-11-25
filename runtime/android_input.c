/* Android input layer. See android_input.h for the modelled scope.
 *
 * The front end (android/jni/port_main.c) translates Android key codes into
 * Win32 virtual keys before it calls the image, but the image also needs the
 * DirectInput scancode view (the game polls GUID_SysKeyboard), so this module
 * owns the AKEYCODE -> DIK / AKEYCODE -> VK tables and the gamepad/touch
 * projections onto them. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "android_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Win32 constants used here ----------------------------------------- */
enum {
    VK_BACK_ = 0x08, VK_TAB_ = 0x09, VK_RETURN_ = 0x0d, VK_SHIFT_ = 0x10,
    VK_CONTROL_ = 0x11, VK_MENU_ = 0x12, VK_PAUSE_ = 0x13, VK_CAPITAL_ = 0x14,
    VK_ESCAPE_ = 0x1b, VK_SPACE_ = 0x20, VK_PRIOR_ = 0x21, VK_NEXT_ = 0x22,
    VK_END_ = 0x23, VK_HOME_ = 0x24, VK_LEFT_ = 0x25, VK_UP_ = 0x26,
    VK_RIGHT_ = 0x27, VK_DOWN_ = 0x28, VK_SNAPSHOT_ = 0x2c, VK_INSERT_ = 0x2d,
    VK_DELETE_ = 0x2e, VK_LWIN_ = 0x5b, VK_RWIN_ = 0x5c, VK_APPS_ = 0x5d,
    VK_MULTIPLY_ = 0x6a, VK_ADD_ = 0x6b, VK_SUBTRACT_ = 0x6d,
    VK_DECIMAL_ = 0x6e, VK_DIVIDE_ = 0x6f, VK_NUMLOCK_ = 0x90,
    VK_SCROLL_ = 0x91, VK_OEM_1_ = 0xba, VK_OEM_PLUS_ = 0xbb,
    VK_OEM_COMMA_ = 0xbc, VK_OEM_MINUS_ = 0xbd, VK_OEM_PERIOD_ = 0xbe,
    VK_OEM_2_ = 0xbf, VK_OEM_3_ = 0xc0, VK_OEM_4_ = 0xdb, VK_OEM_5_ = 0xdc,
    VK_OEM_6_ = 0xdd, VK_OEM_7_ = 0xde
};

typedef struct {
    int32_t akeycode;
    uint32_t dik;
    uint32_t vk;
} AinKeyMap;

/* AKEYCODE -> DIK scancode -> Win32 virtual key. Dik 0 means "no scancode"
 * (Windows has no DIK for it); the VK is still delivered to user32. */
static const AinKeyMap ain_key_map[] = {
    /* letters A..Z: AKEYCODE_A(29)..AKEYCODE_Z(54), DIK 0x1E.., VK 'A'.. */
    { 29, 0x1e, 'A' }, { 30, 0x30, 'B' }, { 31, 0x2e, 'C' }, { 32, 0x20, 'D' },
    { 33, 0x12, 'E' }, { 34, 0x21, 'F' }, { 35, 0x22, 'G' }, { 36, 0x23, 'H' },
    { 37, 0x17, 'I' }, { 38, 0x24, 'J' }, { 39, 0x25, 'K' }, { 40, 0x26, 'L' },
    { 41, 0x32, 'M' }, { 42, 0x31, 'N' }, { 43, 0x18, 'O' }, { 44, 0x19, 'P' },
    { 45, 0x10, 'Q' }, { 46, 0x13, 'R' }, { 47, 0x1f, 'S' }, { 48, 0x14, 'T' },
    { 49, 0x16, 'U' }, { 50, 0x2f, 'V' }, { 51, 0x11, 'W' }, { 52, 0x2d, 'X' },
    { 53, 0x15, 'Y' }, { 54, 0x2c, 'Z' },
    /* digits 0..9 (AKEYCODE_0 = 7 .. AKEYCODE_9 = 16) */
    { 7, 0x0b, '0' }, { 8, 0x02, '1' }, { 9, 0x03, '2' }, { 10, 0x04, '3' },
    { 11, 0x05, '4' }, { 12, 0x06, '5' }, { 13, 0x07, '6' }, { 14, 0x08, '7' },
    { 15, 0x09, '8' }, { 16, 0x0a, '9' },
    /* function keys F1..F12 */
    { 131, 0x3b, 0x70 }, { 132, 0x3c, 0x71 }, { 133, 0x3d, 0x72 },
    { 134, 0x3e, 0x73 }, { 135, 0x3f, 0x74 }, { 136, 0x40, 0x75 },
    { 137, 0x41, 0x76 }, { 138, 0x42, 0x77 }, { 139, 0x43, 0x78 },
    { 140, 0x44, 0x79 }, { 141, 0x57, 0x7a }, { 142, 0x58, 0x7b },
    /* arrows / DPAD */
    { 19, 0xc8, VK_UP_ }, { 20, 0xd0, VK_DOWN_ }, { 21, 0xcb, VK_LEFT_ },
    { 22, 0xcd, VK_RIGHT_ },
    /* editing / navigation. ENTER comes before DPAD_CENTER/NUMPAD_ENTER: the
     * VK -> DIK lookup takes the first row, and VK_RETURN must be the main
     * Enter key (DIK_RETURN), which is what key bindings name. */
    { 66, 0x1c, VK_RETURN_ }, { 23, 0x9c, VK_RETURN_ }, /* DPAD_CENTER */
    { 111, 0x01, VK_ESCAPE_ }, { 62, 0x39, VK_SPACE_ },
    { 61, 0x0f, VK_TAB_ }, { 67, 0x0e, VK_BACK_ }, { 112, 0xd3, VK_DELETE_ },
    { 124, 0xd2, VK_INSERT_ }, { 122, 0xc7, VK_HOME_ }, { 123, 0xcf, VK_END_ },
    { 92, 0xc9, VK_PRIOR_ }, { 93, 0xd1, VK_NEXT_ },
    /* modifiers */
    { 59, 0x2a, VK_SHIFT_ }, { 60, 0x36, VK_SHIFT_ },
    { 113, 0x1d, VK_CONTROL_ }, { 114, 0x9d, VK_CONTROL_ },
    { 57, 0x38, VK_MENU_ }, { 58, 0xb8, VK_MENU_ },
    { 117, 0xdb, VK_LWIN_ }, { 118, 0xdc, VK_RWIN_ },
    { 115, 0x3a, VK_CAPITAL_ }, { 143, 0x45, VK_NUMLOCK_ },
    { 116, 0x46, VK_SCROLL_ },
    /* punctuation */
    { 69, 0x0c, VK_OEM_MINUS_ }, { 70, 0x0d, VK_OEM_PLUS_ },
    { 71, 0x1a, VK_OEM_4_ }, { 72, 0x1b, VK_OEM_6_ },
    { 73, 0x2b, VK_OEM_5_ }, { 74, 0x27, VK_OEM_1_ },
    { 75, 0x28, VK_OEM_7_ }, { 76, 0x35, VK_OEM_2_ },
    { 55, 0x33, VK_OEM_COMMA_ }, { 56, 0x34, VK_OEM_PERIOD_ },
    { 68, 0x29, VK_OEM_3_ },
    /* numpad */
    { 144, 0x52, 0x60 }, { 145, 0x4f, 0x61 }, { 146, 0x50, 0x62 },
    { 147, 0x51, 0x63 }, { 148, 0x4b, 0x64 }, { 149, 0x4c, 0x65 },
    { 150, 0x4d, 0x66 }, { 151, 0x47, 0x67 }, { 152, 0x48, 0x68 },
    { 153, 0x49, 0x69 }, { 154, 0xb5, VK_DIVIDE_ },
    { 155, 0x37, VK_MULTIPLY_ }, { 156, 0x4a, VK_SUBTRACT_ },
    { 157, 0x4e, VK_ADD_ }, { 158, 0x53, VK_DECIMAL_ },
    { 160, 0x9c, VK_RETURN_ },
    /* system */
    { 82, 0xdd, VK_APPS_ }, { 120, 0xb7, VK_SNAPSHOT_ },
    { 121, 0, VK_PAUSE_ }
};

static const uint32_t ain_key_map_count =
    (uint32_t)(sizeof(ain_key_map) / sizeof(ain_key_map[0]));

typedef struct {
    const char *name;
    uint32_t vk;
} AinVkName;

static const AinVkName ain_vk_names[] = {
    { "BACK", VK_BACK_ }, { "BACKSPACE", VK_BACK_ }, { "TAB", VK_TAB_ },
    { "RETURN", VK_RETURN_ }, { "ENTER", VK_RETURN_ },
    { "SHIFT", VK_SHIFT_ }, { "LSHIFT", VK_SHIFT_ }, { "RSHIFT", VK_SHIFT_ },
    { "CONTROL", VK_CONTROL_ }, { "CTRL", VK_CONTROL_ },
    { "LCTRL", VK_CONTROL_ }, { "RCTRL", VK_CONTROL_ },
    { "MENU", VK_MENU_ }, { "ALT", VK_MENU_ }, { "LALT", VK_MENU_ },
    { "RALT", VK_MENU_ }, { "PAUSE", VK_PAUSE_ }, { "CAPITAL", VK_CAPITAL_ },
    { "CAPSLOCK", VK_CAPITAL_ }, { "ESCAPE", VK_ESCAPE_ }, { "ESC", VK_ESCAPE_ },
    { "SPACE", VK_SPACE_ }, { "PRIOR", VK_PRIOR_ }, { "PAGEUP", VK_PRIOR_ },
    { "NEXT", VK_NEXT_ }, { "PAGEDOWN", VK_NEXT_ }, { "END", VK_END_ },
    { "HOME", VK_HOME_ }, { "LEFT", VK_LEFT_ }, { "UP", VK_UP_ },
    { "RIGHT", VK_RIGHT_ }, { "DOWN", VK_DOWN_ }, { "SNAPSHOT", VK_SNAPSHOT_ },
    { "INSERT", VK_INSERT_ }, { "DELETE", VK_DELETE_ }, { "DEL", VK_DELETE_ },
    { "LWIN", VK_LWIN_ }, { "RWIN", VK_RWIN_ }, { "APPS", VK_APPS_ },
    { "MULTIPLY", VK_MULTIPLY_ }, { "ADD", VK_ADD_ },
    { "SUBTRACT", VK_SUBTRACT_ }, { "DECIMAL", VK_DECIMAL_ },
    { "DIVIDE", VK_DIVIDE_ }, { "NUMLOCK", VK_NUMLOCK_ },
    { "SCROLL", VK_SCROLL_ }, { "OEM_1", VK_OEM_1_ }, { "SEMICOLON", VK_OEM_1_ },
    { "OEM_PLUS", VK_OEM_PLUS_ }, { "EQUALS", VK_OEM_PLUS_ },
    { "OEM_COMMA", VK_OEM_COMMA_ }, { "COMMA", VK_OEM_COMMA_ },
    { "OEM_MINUS", VK_OEM_MINUS_ }, { "MINUS", VK_OEM_MINUS_ },
    { "OEM_PERIOD", VK_OEM_PERIOD_ }, { "PERIOD", VK_OEM_PERIOD_ },
    { "OEM_2", VK_OEM_2_ }, { "SLASH", VK_OEM_2_ },
    { "OEM_3", VK_OEM_3_ }, { "GRAVE", VK_OEM_3_ },
    { "OEM_4", VK_OEM_4_ }, { "LBRACKET", VK_OEM_4_ },
    { "OEM_5", VK_OEM_5_ }, { "BACKSLASH", VK_OEM_5_ },
    { "OEM_6", VK_OEM_6_ }, { "RBRACKET", VK_OEM_6_ },
    { "OEM_7", VK_OEM_7_ }, { "APOSTROPHE", VK_OEM_7_ },
    { "NUMPAD0", 0x60 }, { "NUMPAD1", 0x61 }, { "NUMPAD2", 0x62 },
    { "NUMPAD3", 0x63 }, { "NUMPAD4", 0x64 }, { "NUMPAD5", 0x65 },
    { "NUMPAD6", 0x66 }, { "NUMPAD7", 0x67 }, { "NUMPAD8", 0x68 },
    { "NUMPAD9", 0x69 },
    { "F1", 0x70 }, { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 },
    { "F5", 0x74 }, { "F6", 0x75 }, { "F7", 0x76 }, { "F8", 0x77 },
    { "F9", 0x78 }, { "F10", 0x79 }, { "F11", 0x7a }, { "F12", 0x7b }
};
static const uint32_t ain_vk_name_count =
    (uint32_t)(sizeof(ain_vk_names) / sizeof(ain_vk_names[0]));

static char ain_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}
static int ain_name_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ain_upper(*a) != ain_upper(*b)) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

uint32_t ain_vk_from_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (ain_name_equal(name, "NONE") || ain_name_equal(name, "OFF") ||
        ain_name_equal(name, "DISABLED"))
        return 0;
    if (name[1] == 0) {
        char c = ain_upper(name[0]);
        if (c >= 'A' && c <= 'Z') return (uint32_t)c;
        if (c >= '0' && c <= '9') return (uint32_t)c;
    }
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        unsigned long value = strtoul(name + 2, NULL, 16);
        if (value > 0 && value <= 0xfe) return (uint32_t)value;
        return 0;
    }
    for (uint32_t i = 0; i < ain_vk_name_count; i++)
        if (ain_name_equal(name, ain_vk_names[i].name))
            return ain_vk_names[i].vk;
    return 0;
}

const char *ain_vk_name(uint32_t vk) {
    for (uint32_t i = 0; i < ain_vk_name_count; i++)
        if (ain_vk_names[i].vk == vk) return ain_vk_names[i].name;
    return "?";
}

uint32_t ain_dik_from_akeycode(int32_t akeycode) {
    for (uint32_t i = 0; i < ain_key_map_count; i++)
        if (ain_key_map[i].akeycode == akeycode) return ain_key_map[i].dik;
    return 0;
}
uint32_t ain_vk_from_akeycode(int32_t akeycode) {
    for (uint32_t i = 0; i < ain_key_map_count; i++)
        if (ain_key_map[i].akeycode == akeycode) return ain_key_map[i].vk;
    return 0;
}
uint32_t ain_dik_from_vk(uint32_t vk) {
    for (uint32_t i = 0; i < ain_key_map_count; i++)
        if (ain_key_map[i].vk == vk && ain_key_map[i].dik) return ain_key_map[i].dik;
    return 0;
}
uint32_t ain_akeycode_count(void) { return ain_key_map_count; }
int32_t ain_akeycode_at(uint32_t index, uint32_t *dik, uint32_t *vk) {
    if (index >= ain_key_map_count) return 0;
    if (dik) *dik = ain_key_map[index].dik;
    if (vk) *vk = ain_key_map[index].vk;
    return ain_key_map[index].akeycode;
}

/* --- touch zones ------------------------------------------------------- */
static const char *const ain_zone_names[AIN_ZONE_COUNT] = {
    "none", "steer-left", "steer-right", "brake", "gas", "menu"
};
const char *ain_zone_name(uint32_t zone) {
    return zone < AIN_ZONE_COUNT ? ain_zone_names[zone] : "?";
}

/* Racing layout: the lower half is the control area. Left half steers
 * (x < w/4 = left, else right), right half pedals (x < 3w/4 = brake, else
 * gas). The upper half is a menu tap zone. */
uint32_t ain_touch_zone(uint32_t x, uint32_t y, uint32_t width,
                        uint32_t height) {
    if (!width || !height) return AIN_ZONE_NONE;
    if (x >= width) x = width - 1;
    if (y >= height) y = height - 1;
    if (y < height / 2u) return AIN_ZONE_MENU;
    if (x < width / 2u) return (x < width / 4u) ? AIN_ZONE_STEER_LEFT
                                                : AIN_ZONE_STEER_RIGHT;
    return (x < (width * 3u) / 4u) ? AIN_ZONE_BRAKE : AIN_ZONE_GAS;
}

/* --- bindings ---------------------------------------------------------- */
/* Pad -> key translation, used only when the image is not given the pad
 * itself (PORT_GAMEPAD=0). The keys are the image's own keyboard defaults
 * ([GlobalConfig] / [DefaultConfig_Keyboard] in its controller table), laid
 * out like its XInput defaults: A select/boost, B cancel, X handbrake,
 * Y camera, RT accelerate, LT brake. */
static const AinBinding ain_default_bindings[] = {
    { "select", AIN_SRC_BUTTON, PORT_PAD_A, 0, VK_RETURN_ },
    { "boost", AIN_SRC_BUTTON, PORT_PAD_A, 0, VK_SHIFT_ },
    { "cancel", AIN_SRC_BUTTON, PORT_PAD_B, 0, VK_BACK_ },
    { "handbrake", AIN_SRC_BUTTON, PORT_PAD_X, 0, VK_CONTROL_ },
    { "camera", AIN_SRC_BUTTON, PORT_PAD_Y, 0, 'C' },
    { "accelerate", AIN_SRC_AXIS_POS, PORT_AXIS_RIGHT_TRIGGER, 350, 'A' },
    { "brake", AIN_SRC_AXIS_POS, PORT_AXIS_LEFT_TRIGGER, 350, 'Z' },
    { "up", AIN_SRC_DPAD, PORT_PAD_DPAD_UP, 0, VK_UP_ },
    { "up_stick", AIN_SRC_AXIS_NEG, PORT_AXIS_LEFT_Y, 500, VK_UP_ },
    { "down", AIN_SRC_DPAD, PORT_PAD_DPAD_DOWN, 0, VK_DOWN_ },
    { "down_stick", AIN_SRC_AXIS_POS, PORT_AXIS_LEFT_Y, 500, VK_DOWN_ },
    { "left", AIN_SRC_DPAD, PORT_PAD_DPAD_LEFT, 0, VK_LEFT_ },
    { "left_stick", AIN_SRC_AXIS_NEG, PORT_AXIS_LEFT_X, 350, VK_LEFT_ },
    { "right", AIN_SRC_DPAD, PORT_PAD_DPAD_RIGHT, 0, VK_RIGHT_ },
    { "right_stick", AIN_SRC_AXIS_POS, PORT_AXIS_LEFT_X, 350, VK_RIGHT_ },
    { "left_shoulder", AIN_SRC_BUTTON, PORT_PAD_LEFT_SHOULDER, 0, 0x70 },
    { "right_shoulder", AIN_SRC_BUTTON, PORT_PAD_RIGHT_SHOULDER, 0, 0x71 },
    { "horn", AIN_SRC_BUTTON, PORT_PAD_LEFT_THUMB, 0, 'P' },
    { "showtime", AIN_SRC_BUTTON, PORT_PAD_RIGHT_THUMB, 0, 'S' },
    { "start", AIN_SRC_BUTTON, PORT_PAD_START, 0, VK_ESCAPE_ },
    { "map", AIN_SRC_BUTTON, PORT_PAD_BACK, 0, 'M' }
};
static const uint32_t ain_default_binding_count =
    (uint32_t)(sizeof(ain_default_bindings) / sizeof(ain_default_bindings[0]));

/* --- instance ---------------------------------------------------------- */
struct AndroidInput {
    GuestDinput *dinput;
    GuestUser32 *user32;
    GdiHostState host;
    /* Per-source reference counts: two keys can share a virtual key (left and
     * right shift) and two sources can hold the same key, so a bitmask alone
     * would clear a key that is still held. */
    uint8_t vk_count[AIN_VK_COUNT][AIN_SRC_COUNT];
    uint8_t dik_count[256][AIN_SRC_COUNT];
    uint32_t vk_holders[AIN_VK_COUNT];
    uint8_t dik[256];
    uint8_t pad_vk[AIN_VK_COUNT];
    uint8_t touch_vk[AIN_VK_COUNT];
    uint32_t pointer_zone[AIN_MAX_POINTERS];
    uint32_t pointer_active[AIN_MAX_POINTERS];
    AinBinding bindings[AIN_MAX_BINDINGS];
    uint32_t binding_count;
    uint32_t zone_vk[AIN_ZONE_COUNT];
    uint32_t zone_hits[AIN_ZONE_COUNT];
    uint32_t surface_w, surface_h;
    uint32_t events_posted;
    uint32_t gamepad_updates;
    uint32_t have_gamepad;
    uint32_t native_pad;   /* the image reads the pad itself: no pad keys */
    port_gamepad_state pad;
    char bindings_path[AIN_PATH_MAX];
    char last_error[160];
};

static void ain_error(AndroidInput *ain, const char *what) {
    if (!ain) return;
    snprintf(ain->last_error, sizeof(ain->last_error), "%s", what);
}

AndroidInput *ain_create(void) {
    AndroidInput *ain = (AndroidInput *)calloc(1, sizeof(*ain));
    if (!ain) return NULL;
    ain->binding_count = ain_default_binding_count;
    for (uint32_t i = 0; i < ain_default_binding_count; i++)
        ain->bindings[i] = ain_default_bindings[i];
    ain->zone_vk[AIN_ZONE_STEER_LEFT] = VK_LEFT_;
    ain->zone_vk[AIN_ZONE_STEER_RIGHT] = VK_RIGHT_;
    ain->zone_vk[AIN_ZONE_BRAKE] = 'Z';   /* the image's keyboard defaults */
    ain->zone_vk[AIN_ZONE_GAS] = 'A';
    ain->zone_vk[AIN_ZONE_MENU] = VK_RETURN_;
    ain->host.present = 1; /* the keyboard always exists */
    ain->host.gamepad_type = GDI_DI8DEVTYPE_GAMEPAD;
    ain->surface_w = 1280;
    ain->surface_h = 720;
    return ain;
}

void ain_destroy(AndroidInput *ain) { free(ain); }

int ain_set_targets(AndroidInput *ain, GuestDinput *dinput,
                    GuestUser32 *user32) {
    if (!ain) return GM_RANGE;
    ain->dinput = dinput;
    ain->user32 = user32;
    if (dinput) {
        int rc = gdi_set_host_state(dinput, &ain->host);
        if (rc) return rc;
    }
    return GM_OK;
}

static int ain_push_host_state(AndroidInput *ain) {
    if (!ain->dinput) return GM_OK;
    return gdi_set_host_state(ain->dinput, &ain->host);
}

static int ain_post(AndroidInput *ain, uint32_t type, uint32_t x, uint32_t y,
                    uint32_t code, uint32_t width, uint32_t height) {
    if (!ain->user32) return GM_OK;
    GuHostInput input;
    memset(&input, 0, sizeof(input));
    input.type = type;
    input.x = x;
    input.y = y;
    input.code = code;
    input.width = width;
    input.height = height;
    int rc = gu_post_host_input(ain->user32, &input);
    if (rc == GM_OK) ain->events_posted++;
    return rc;
}

/* One virtual-key transition: updates the DIK scancode view and posts the
 * user32 message only when the key really changes state. */
static void ain_transition(AndroidInput *ain, uint32_t vk, uint32_t dik,
                           uint32_t source, uint32_t down) {
    if (!ain || !vk || vk >= AIN_VK_COUNT) return;
    if (source >= AIN_SRC_COUNT) return;
    uint32_t bit = 1u << source;
    uint32_t before = ain->vk_holders[vk];
    uint8_t *count = &ain->vk_count[vk][source];
    if (down) {
        /* A key that is already down stays down once: auto-repeat (and an
         * injected long press) sends several downs for one up. Pad and touch
         * sources can hold one key through several controls and are counted. */
        if (source == AIN_SRC_KEYBOARD) *count = 1;
        else if (*count < 255) (*count)++;
    } else if (*count) {
        (*count)--;
    }
    uint32_t after = 0;
    for (uint32_t i = 0; i < AIN_SRC_COUNT; i++)
        if (ain->vk_count[vk][i]) after |= 1u << i;
    int transition = (before != 0) != (after != 0);
    ain->vk_holders[vk] = after;
    /* The scancode view is updated even when the virtual key does not change:
     * left and right shift share VK_SHIFT but have their own DIK codes, and
     * DirectInput (the game's input path) reads the scancodes. */
    if (!dik) dik = ain_dik_from_vk(vk);
    int dik_changed = 0;
    if (dik && dik < 256) {
        uint8_t *dcount = &ain->dik_count[dik][source];
        uint8_t dik_before = *dcount;
        if (down) {
            if (source == AIN_SRC_KEYBOARD) *dcount = 1;
            else if (*dcount < 255) (*dcount)++;
        } else if (*dcount) {
            (*dcount)--;
        }
        if (*dcount != dik_before) {
            uint32_t mask = 0;
            for (uint32_t i = 0; i < AIN_SRC_COUNT; i++)
                if (ain->dik_count[dik][i]) mask |= 1u << i;
            ain->dik[dik] = mask ? 1 : 0;
            memcpy(ain->host.keys, ain->dik, sizeof(ain->host.keys));
            dik_changed = 1;
        }
    }
    (void)bit;
    if (!transition) {
        if (dik_changed) (void)ain_push_host_state(ain);
        return;
    }
    (void)ain_post(ain, down ? GU_INPUT_KEY_DOWN : GU_INPUT_KEY_UP, 0, 0, vk,
                   0, 0);
    (void)ain_push_host_state(ain);
}

/* --- gamepad ----------------------------------------------------------- */
static int32_t ain_axis_milli(const port_gamepad_state *pad, uint32_t axis) {
    if (!pad || axis >= PORT_AXIS_COUNT) return 0;
    float value = pad->axis[axis];
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return (int32_t)(value * 1000.0f);
}

static int ain_binding_active(const AinBinding *binding,
                              const port_gamepad_state *pad) {
    if (!binding->vk || !pad || !pad->connected) return 0;
    switch (binding->source) {
    case AIN_SRC_BUTTON:
        return binding->index < PORT_PAD_BUTTON_COUNT &&
               (pad->buttons >> binding->index) & 1u;
    case AIN_SRC_DPAD:
        return binding->index < 4 &&
               (pad->dpad >> binding->index) & 1u;
    case AIN_SRC_AXIS_POS:
        return ain_axis_milli(pad, binding->index) > binding->threshold;
    case AIN_SRC_AXIS_NEG:
        return ain_axis_milli(pad, binding->index) < -binding->threshold;
    default:
        return 0;
    }
}

/* Recomputes the gamepad-derived virtual keys from the whole pad state, so
 * two bindings on the same key never clear each other. */
static void ain_apply_pad_keys(AndroidInput *ain) {
    uint8_t next[AIN_VK_COUNT];
    memset(next, 0, sizeof(next));
    for (uint32_t i = 0; i < ain->binding_count; i++) {
        const AinBinding *binding = &ain->bindings[i];
        if (!binding->vk || binding->vk >= AIN_VK_COUNT) continue;
        if (ain_binding_active(binding, &ain->pad)) next[binding->vk] = 1;
    }
    for (uint32_t vk = 1; vk < AIN_VK_COUNT; vk++) {
        if (next[vk] == ain->pad_vk[vk]) continue;
        ain->pad_vk[vk] = next[vk];
        ain_transition(ain, vk, 0, AIN_SRC_GAMEPAD, next[vk]);
    }
}

static int32_t ain_scale_axis(float value) {
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    double scaled = (double)value * 32767.0;
    return (int32_t)(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
}
static int32_t ain_scale_trigger(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return (int32_t)((double)value * 32767.0 + 0.5);
}

static void ain_fill_joystick(AndroidInput *ain) {
    GdiHostState *host = &ain->host;
    const port_gamepad_state *pad = &ain->pad;
    if (!pad->connected) {
        /* A natively presented pad stays attached (it is the device's own). */
        if (!ain->native_pad) host->present &= ~4u;
        memset(host->axis, 0, sizeof(host->axis));
        for (uint32_t i = 0; i < 4; i++) host->pov[i] = 0xffffffffu;
        memset(host->buttons, 0, sizeof(host->buttons));
        return;
    }
    host->present |= 4u;
    host->gamepad_type = GDI_DI8DEVTYPE_GAMEPAD;
    host->axis[0] = ain_scale_axis(pad->axis[PORT_AXIS_LEFT_X]);
    host->axis[1] = ain_scale_axis(pad->axis[PORT_AXIS_LEFT_Y]);
    host->axis[2] = ain_scale_trigger(pad->axis[PORT_AXIS_LEFT_TRIGGER]);
    host->axis[3] = ain_scale_axis(pad->axis[PORT_AXIS_RIGHT_X]);
    host->axis[4] = ain_scale_axis(pad->axis[PORT_AXIS_RIGHT_Y]);
    host->axis[5] = ain_scale_trigger(pad->axis[PORT_AXIS_RIGHT_TRIGGER]);
    /* POV: hundredths of a degree clockwise from up, 0xffffffff = centred. */
    uint32_t dpad = pad->dpad;
    uint32_t up = (dpad >> PORT_PAD_DPAD_UP) & 1u;
    uint32_t down = (dpad >> PORT_PAD_DPAD_DOWN) & 1u;
    uint32_t left = (dpad >> PORT_PAD_DPAD_LEFT) & 1u;
    uint32_t right = (dpad >> PORT_PAD_DPAD_RIGHT) & 1u;
    if (!up && !down && !left && !right) {
        host->pov[0] = 0xffffffffu;
    } else {
        int32_t angle;
        if (up && right) angle = 4500;
        else if (right && down) angle = 13500;
        else if (down && left) angle = 22500;
        else if (left && up) angle = 31500;
        else if (up) angle = 0;
        else if (right) angle = 9000;
        else if (down) angle = 18000;
        else angle = 27000;
        host->pov[0] = (uint32_t)angle;
    }
    for (uint32_t i = 1; i < 4; i++) host->pov[i] = 0xffffffffu;
    memset(host->buttons, 0, sizeof(host->buttons));
    /* XInput button order: A B X Y LB RB BACK START LTHUMB RTHUMB. */
    static const uint32_t order[10] = {
        PORT_PAD_A, PORT_PAD_B, PORT_PAD_X, PORT_PAD_Y,
        PORT_PAD_LEFT_SHOULDER, PORT_PAD_RIGHT_SHOULDER,
        PORT_PAD_BACK, PORT_PAD_START,
        PORT_PAD_LEFT_THUMB, PORT_PAD_RIGHT_THUMB
    };
    for (uint32_t i = 0; i < 10; i++)
        host->buttons[i] = (uint8_t)((pad->buttons >> order[i]) & 1u);
}

int ain_set_gamepad_state(AndroidInput *ain, const port_gamepad_state *state) {
    if (!ain || !state) return GM_RANGE;
    if (state->abi_version != PORT_HOST_API_ABI) {
        ain_error(ain, "gamepad state ABI mismatch");
        return GM_UNSUPPORTED;
    }
    memcpy(&ain->pad, state, sizeof(ain->pad));
    ain->have_gamepad = state->connected ? 1u : 0u;
    ain->gamepad_updates++;
    ain_fill_joystick(ain);
    ain_apply_pad_keys(ain);
    int rc = ain_push_host_state(ain);
    return rc;
}

/* --- keyboard ---------------------------------------------------------- */
int ain_key_event(AndroidInput *ain, int32_t akeycode, uint32_t down) {
    if (!ain) return GM_RANGE;
    uint32_t vk = ain_vk_from_akeycode(akeycode);
    uint32_t dik = ain_dik_from_akeycode(akeycode);
    if (!vk) {
        /* Unknown key: the front end already logs it; nothing is invented. */
        ain_error(ain, "key code outside the mapped table");
        return GM_UNSUPPORTED;
    }
    ain_transition(ain, vk, dik, AIN_SRC_KEYBOARD, down ? 1u : 0u);
    return GM_OK;
}

/* --- touch ------------------------------------------------------------- */
static void ain_apply_touch_keys(AndroidInput *ain) {
    uint8_t next[AIN_VK_COUNT];
    memset(next, 0, sizeof(next));
    for (uint32_t i = 0; i < AIN_MAX_POINTERS; i++) {
        if (!ain->pointer_active[i]) continue;
        uint32_t zone = ain->pointer_zone[i];
        if (zone >= AIN_ZONE_COUNT) continue;
        uint32_t vk = ain->zone_vk[zone];
        if (vk && vk < AIN_VK_COUNT) next[vk] = 1;
    }
    for (uint32_t vk = 1; vk < AIN_VK_COUNT; vk++) {
        if (next[vk] == ain->touch_vk[vk]) continue;
        ain->touch_vk[vk] = next[vk];
        ain_transition(ain, vk, 0, AIN_SRC_TOUCH, next[vk]);
    }
}

int ain_touch_event(AndroidInput *ain, uint32_t action, uint32_t pointer,
                    uint32_t x, uint32_t y) {
    if (!ain) return GM_RANGE;
    if (pointer >= AIN_MAX_POINTERS) return GM_UNSUPPORTED;
    uint32_t zone = ain_touch_zone(x, y, ain->surface_w, ain->surface_h);
    switch (action) {
    case 0: /* ACTION_DOWN */
    case 5: /* ACTION_POINTER_DOWN */
        ain->pointer_active[pointer] = 1;
        ain->pointer_zone[pointer] = zone;
        if (zone < AIN_ZONE_COUNT && zone != AIN_ZONE_NONE)
            ain->zone_hits[zone]++;
        ain_apply_touch_keys(ain);
        (void)ain_post(ain, GU_INPUT_TOUCH_DOWN, x, y, 0, 0, 0);
        break;
    case 2: /* ACTION_MOVE */
        if (!ain->pointer_active[pointer]) {
            ain->pointer_active[pointer] = 1;
        }
        if (ain->pointer_zone[pointer] != zone) {
            ain->pointer_zone[pointer] = zone;
            if (zone < AIN_ZONE_COUNT && zone != AIN_ZONE_NONE)
                ain->zone_hits[zone]++;
            ain_apply_touch_keys(ain);
        }
        (void)ain_post(ain, GU_INPUT_TOUCH_MOVE, x, y, 0, 0, 0);
        break;
    case 1: /* ACTION_UP */
    case 6: /* ACTION_POINTER_UP */
    case 3: /* ACTION_CANCEL */
        ain->pointer_active[pointer] = 0;
        ain->pointer_zone[pointer] = AIN_ZONE_NONE;
        ain_apply_touch_keys(ain);
        (void)ain_post(ain, GU_INPUT_TOUCH_UP, x, y, 0, 0, 0);
        break;
    default:
        return GM_UNSUPPORTED;
    }
    return GM_OK;
}

/* --- window / lifecycle ------------------------------------------------ */
int ain_set_surface_size(AndroidInput *ain, uint32_t width, uint32_t height) {
    if (!ain) return GM_RANGE;
    if (!width || !height) return GM_UNSUPPORTED;
    ain->surface_w = width;
    ain->surface_h = height;
    return ain_post(ain, GU_INPUT_RESIZE, 0, 0, 0, width, height);
}

int ain_focus_event(AndroidInput *ain, uint32_t gained) {
    if (!ain) return GM_RANGE;
    if (!gained) (void)ain_release_all(ain);
    return ain_post(ain, gained ? GU_INPUT_FOCUS_GAINED : GU_INPUT_FOCUS_LOST,
                    0, 0, 0, 0, 0);
}

int ain_close_event(AndroidInput *ain) {
    if (!ain) return GM_RANGE;
    return ain_post(ain, GU_INPUT_CLOSE, 0, 0, 0, 0, 0);
}

int ain_release_all(AndroidInput *ain) {
    if (!ain) return GM_RANGE;
    for (uint32_t vk = 1; vk < AIN_VK_COUNT; vk++) {
        if (!ain->vk_holders[vk]) continue;
        ain->vk_holders[vk] = 0;
        memset(ain->vk_count[vk], 0, sizeof(ain->vk_count[vk]));
        uint32_t dik = ain_dik_from_vk(vk);
        if (dik && dik < 256) {
            memset(ain->dik_count[dik], 0, sizeof(ain->dik_count[dik]));
            ain->dik[dik] = 0;
        }
        (void)ain_post(ain, GU_INPUT_KEY_UP, 0, 0, vk, 0, 0);
    }
    memset(ain->pad_vk, 0, sizeof(ain->pad_vk));
    memset(ain->touch_vk, 0, sizeof(ain->touch_vk));
    for (uint32_t i = 0; i < AIN_MAX_POINTERS; i++) {
        ain->pointer_active[i] = 0;
        ain->pointer_zone[i] = AIN_ZONE_NONE;
    }
    memset(ain->dik, 0, sizeof(ain->dik));
    memset(ain->dik_count, 0, sizeof(ain->dik_count));
    memset(ain->vk_count, 0, sizeof(ain->vk_count));
    memcpy(ain->host.keys, ain->dik, sizeof(ain->host.keys));
    return ain_push_host_state(ain);
}

/* --- configuration ----------------------------------------------------- */
static char *ain_trim(char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    size_t length = strlen(text);
    while (length) {
        char last = text[length - 1];
        if (last != ' ' && last != '\t' && last != '\r' && last != '\n') break;
        text[--length] = 0;
    }
    return text;
}

int ain_load_bindings(AndroidInput *ain, const char *path) {
    if (!ain || !path) return GM_RANGE;
    FILE *file = fopen(path, "rb");
    if (!file) {
        /* No override file: the built-in racing layout stays in place. */
        ain->bindings_path[0] = 0;
        return GM_OK;
    }
    snprintf(ain->bindings_path, sizeof(ain->bindings_path), "%s", path);
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *text = ain_trim(line);
        if (!text[0] || text[0] == '#' || text[0] == ';') continue;
        char *equals = strchr(text, '=');
        if (!equals) {
            ain_error(ain, "gamepad-keys.ini: line without '='");
            continue;
        }
        *equals = 0;
        char *key = ain_trim(text);
        char *value = ain_trim(equals + 1);
        uint32_t vk = ain_vk_from_name(value);
        if (!vk && !ain_name_equal(value, "NONE") &&
            !ain_name_equal(value, "OFF") && !ain_name_equal(value, "0")) {
            ain_error(ain, "gamepad-keys.ini: unknown key name");
            continue;
        }
        int found = 0;
        for (uint32_t i = 0; i < ain->binding_count; i++) {
            if (!ain_name_equal(key, ain->bindings[i].name)) continue;
            ain->bindings[i].vk = vk;
            found = 1;
            break;
        }
        if (!found) ain_error(ain, "gamepad-keys.ini: unknown binding name");
    }
    fclose(file);
    /* Re-evaluate the pad projection: a rebound key must not stay stuck. */
    ain_apply_pad_keys(ain);
    return GM_OK;
}

int ain_set_user_root(AndroidInput *ain, const char *user_root) {
    if (!ain || !user_root) return GM_RANGE;
    char path[AIN_PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/gamepad-keys.ini", user_root);
    if (written <= 0 || (size_t)written >= sizeof(path)) return GM_RANGE;
    return ain_load_bindings(ain, path);
}

const AinBinding *ain_bindings(const AndroidInput *ain, uint32_t *count) {
    if (!ain) { if (count) *count = 0; return NULL; }
    if (count) *count = ain->binding_count;
    return ain->bindings;
}

int ain_set_binding_vk(AndroidInput *ain, uint32_t index, uint32_t vk) {
    if (!ain || index >= ain->binding_count || vk >= AIN_VK_COUNT)
        return GM_RANGE;
    ain->bindings[index].vk = vk;
    ain_apply_pad_keys(ain);
    return GM_OK;
}

uint32_t ain_zone_vk(const AndroidInput *ain, uint32_t zone) {
    if (!ain || zone >= AIN_ZONE_COUNT) return 0;
    return ain->zone_vk[zone];
}
int ain_set_zone_vk(AndroidInput *ain, uint32_t zone, uint32_t vk) {
    if (!ain || zone >= AIN_ZONE_COUNT || vk >= AIN_VK_COUNT) return GM_RANGE;
    ain->zone_vk[zone] = vk;
    return GM_OK;
}

/* --- diagnostics ------------------------------------------------------- */
const GdiHostState *ain_host_state(const AndroidInput *ain) {
    return ain ? &ain->host : NULL;
}
uint32_t ain_vk_holders(const AndroidInput *ain, uint32_t vk) {
    return (ain && vk < AIN_VK_COUNT) ? ain->vk_holders[vk] : 0u;
}
uint32_t ain_dik_state(const AndroidInput *ain, uint32_t dik) {
    return (ain && dik < 256) ? ain->dik[dik] : 0u;
}
uint32_t ain_events_posted(const AndroidInput *ain) {
    return ain ? ain->events_posted : 0u;
}
uint32_t ain_gamepad_updates(const AndroidInput *ain) {
    return ain ? ain->gamepad_updates : 0u;
}
uint32_t ain_zone_hits(const AndroidInput *ain, uint32_t zone) {
    return (ain && zone < AIN_ZONE_COUNT) ? ain->zone_hits[zone] : 0u;
}
uint32_t ain_surface_width(const AndroidInput *ain) {
    return ain ? ain->surface_w : 0u;
}
uint32_t ain_surface_height(const AndroidInput *ain) {
    return ain ? ain->surface_h : 0u;
}
const char *ain_last_error(const AndroidInput *ain) {
    return (ain && ain->last_error[0]) ? ain->last_error : NULL;
}
const char *ain_bindings_path(const AndroidInput *ain) {
    return (ain && ain->bindings_path[0]) ? ain->bindings_path : NULL;
}

/* --- global hooks ------------------------------------------------------ */
static AndroidInput *ain_default;
#if defined(__ANDROID__)
#include <pthread.h>
static pthread_mutex_t ain_default_lock = PTHREAD_MUTEX_INITIALIZER;
static void ain_lock(void) { pthread_mutex_lock(&ain_default_lock); }
static void ain_unlock(void) { pthread_mutex_unlock(&ain_default_lock); }
#else
/* Single-threaded host tests: no lock is needed. */
static void ain_lock(void) { }
static void ain_unlock(void) { }
#endif

AndroidInput *ain_default_instance(void) {
    ain_lock();
    if (!ain_default) ain_default = ain_create();
    AndroidInput *instance = ain_default;
    ain_unlock();
    return instance;
}

int ain_default_set_targets(GuestDinput *dinput, GuestUser32 *user32) {
    AndroidInput *ain = ain_default_instance();
    if (!ain) return GM_ALLOCATION;
    return ain_set_targets(ain, dinput, user32);
}

/* Native gamepad: the image is shown an attached controller and reads it
 * itself, so the pad is always "present" for DirectInput and no pad control is
 * turned into a key (gamepad-keys.ini can still assign keys to bindings). */
int ain_default_set_native_gamepad(int enabled) {
    AndroidInput *ain = ain_default_instance();
    if (!ain) return GM_ALLOCATION;
    ain->native_pad = enabled ? 1u : 0u;
    if (enabled) {
        for (uint32_t i = 0; i < ain->binding_count; i++) ain->bindings[i].vk = 0;
        ain->host.present |= 4u;
        for (uint32_t i = 0; i < 4; i++) ain->host.pov[i] = 0xffffffffu;
        ain_apply_pad_keys(ain);
    }
    return GM_OK;
}

/* An image that reads the pad itself (XInput) takes the state as it is. */
static int (*ain_gamepad_sink)(const port_gamepad_state *state);
void ain_set_gamepad_sink(int (*sink)(const port_gamepad_state *state)) { ain_gamepad_sink = sink; }

int port_set_gamepad_state(const port_gamepad_state *state) {
    /* The sink (XInput) gets the raw state. The key translation below only
     * has bindings when the pad is not presented natively. */
    if (ain_gamepad_sink && state) ain_gamepad_sink(state);
    AndroidInput *ain = ain_default_instance();
    if (!ain) return GM_ALLOCATION;
    return ain_set_gamepad_state(ain, state);
}

int port_post_host_input(uint32_t type, uint32_t x, uint32_t y, uint32_t code) {
    AndroidInput *ain = ain_default_instance();
    if (!ain) return GM_ALLOCATION;
    switch (type) {
    case GU_INPUT_TOUCH_DOWN:
        return ain_touch_event(ain, 0, 0, x, y);
    case GU_INPUT_TOUCH_UP:
        return ain_touch_event(ain, 1, 0, x, y);
    case GU_INPUT_TOUCH_MOVE:
        return ain_touch_event(ain, 2, 0, x, y);
    case GU_INPUT_KEY_DOWN:
    case GU_INPUT_KEY_UP:
        /* The front end already converted the Android key code to a VK. */
        ain_transition(ain, code, 0, AIN_SRC_KEYBOARD,
                       type == GU_INPUT_KEY_DOWN ? 1u : 0u);
        return GM_OK;
    case GU_INPUT_RESIZE:
        return ain_set_surface_size(ain, x, y);
    case GU_INPUT_CLOSE:
        return ain_close_event(ain);
    case GU_INPUT_FOCUS_GAINED:
        return ain_focus_event(ain, 1);
    case GU_INPUT_FOCUS_LOST:
        return ain_focus_event(ain, 0);
    default:
        return GM_UNSUPPORTED;
    }
}

int port_set_surface_size(int width, int height) {
    AndroidInput *ain = ain_default_instance();
    if (!ain) return GM_ALLOCATION;
    if (width <= 0 || height <= 0) return GM_UNSUPPORTED;
    return ain_set_surface_size(ain, (uint32_t)width, (uint32_t)height);
}
