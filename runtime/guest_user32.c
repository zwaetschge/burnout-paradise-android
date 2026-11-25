/* user32 window/message model: one top-level game window on one host surface.
 *
 * Scope and deliberate limits (all stops are named, never silent):
 *   - one top-level window, no child/owned windows, no desktop, no window
 *     manager, no window station;
 *   - one message queue (the single guest thread), thread safe for host input;
 *   - no resource tree: named cursors/icons and MessageBox text are logged,
 *     predefined integer resources are returned unchanged;
 *   - no GDI/DC, no update regions, no timers, no synthesised input;
 *   - guest text is ASCII only (both A and W variants), everything else is a
 *     named stop instead of a lossy conversion.
 * Guest memory is only touched through gm_check/gm_read/gm_write. */
#include "guest_user32.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Win32 constants used by this model --------------------------------- */
enum {
    WM_NULL_ = 0x0000, WM_CREATE_ = 0x0001, WM_DESTROY_ = 0x0002,
    WM_MOVE_ = 0x0003, WM_SIZE_ = 0x0005, WM_ACTIVATE_ = 0x0006,
    WM_SETFOCUS_ = 0x0007,
    WM_KILLFOCUS_ = 0x0008, WM_PAINT_ = 0x000f, WM_CLOSE_ = 0x0010,
    WM_QUIT_ = 0x0012, WM_ERASEBKGND_ = 0x0014, WM_SHOWWINDOW_ = 0x0018,
    WM_ACTIVATEAPP_ = 0x001c, WM_SETCURSOR_ = 0x0020, WM_GETMINMAXINFO_ = 0x0024,
    WM_DISPLAYCHANGE_ = 0x007e, WM_NCCREATE_ = 0x0081, WM_NCDESTROY_ = 0x0082,
    WM_NCCALCSIZE_ = 0x0083, WM_NCHITTEST_ = 0x0084,
    WM_KEYDOWN_ = 0x0100, WM_KEYUP_ = 0x0101, WM_CHAR_ = 0x0102,
    WM_SYSKEYDOWN_ = 0x0104, WM_SYSKEYUP_ = 0x0105, WM_SYSCHAR_ = 0x0106,
    WM_SYSCOMMAND_ = 0x0112, WM_MOUSEMOVE_ = 0x0200, WM_LBUTTONDOWN_ = 0x0201,
    WM_LBUTTONUP_ = 0x0202, WM_RBUTTONDOWN_ = 0x0204, WM_RBUTTONUP_ = 0x0205,
    WM_MOUSEWHEEL_ = 0x020a
};
enum {
    WS_CAPTION_ = 0x00c00000u, WS_THICKFRAME_ = 0x00040000u,
    WS_BORDER_ = 0x00800000u, WS_DLGFRAME_ = 0x00400000u,
    WS_POPUP_ = 0x80000000u, WS_CHILD_ = 0x40000000u, WS_VISIBLE_ = 0x10000000u,
    WS_EX_WINDOWEDGE_ = 0x00000100u, WS_EX_CLIENTEDGE_ = 0x00000200u
};
enum {
    GWL_WNDPROC_ = (uint32_t)-4, GWL_HINSTANCE_ = (uint32_t)-6,
    GWL_HWNDPARENT_ = (uint32_t)-8, GWL_ID_ = (uint32_t)-12,
    GWL_STYLE_ = (uint32_t)-16, GWL_EXSTYLE_ = (uint32_t)-20,
    GWL_USERDATA_ = (uint32_t)-21
};
enum {
    ERROR_INVALID_FUNCTION_ = 1, ERROR_NOT_ENOUGH_MEMORY_ = 8,
    ERROR_INVALID_PARAMETER_ = 87, ERROR_INVALID_WINDOW_HANDLE_ = 1400,
    ERROR_CANNOT_FIND_WND_CLASS_ = 1407, ERROR_CLASS_ALREADY_EXISTS_ = 1410
};
/* Window frame metrics of the modelled surface. A fullscreen game window has
 * no frame; the border/caption values below are only used by
 * AdjustWindowRect(Ex) and GetSystemMetrics for the documented metrics. */
enum { GU_FRAME_BORDER = 8, GU_FRAME_CAPTION = 23, GU_FRAME_MENU = 19,
       GU_SM_CXICON = 32, GU_SM_CYICON = 32, GU_SM_CXCURSOR = 32,
       GU_SM_CYCURSOR = 32, GU_SM_CXSMICON = 16, GU_SM_CYSMICON = 16 };
#define CW_USEDEFAULT_ 0x80000000u
#define ENUM_CURRENT_SETTINGS_ 0xffffffffu
#define ENUM_REGISTRY_SETTINGS_ 0xfffffffeu

typedef struct {
    int used;
    uint32_t atom;
    uint32_t style, wndproc, cls_extra, wnd_extra, instance;
    uint32_t icon, cursor, background, menu_name, icon_small;
    char name[GU_CLASS_NAME_MAX];
} GuWindowClass;

typedef struct {
    int used;
    uint32_t hwnd;
    int class_index;
    uint32_t style, exstyle, instance, menu, userdata, wndproc;
    uint32_t x, y, w, h;
    uint32_t client_w, client_h;
    int visible;
    int active;      /* the application currently owns the activation (WM_ACTIVATEAPP sent) */
    char title[GU_WINDOW_TEXT_MAX];
} GuWindow;

struct GuestUser32 {
    GuestMemory *memory;
    uint32_t teb;
    pthread_mutex_t lock;
    uint32_t surface_w, surface_h;
    GuGuestCall call;
    void *call_context;
    GuMessageWait wait;
    void *wait_context;
    uint32_t scratch, scratch_size;
    GuWindowClass classes[GU_MAX_CLASSES];
    uint32_t class_count;
    GuWindow window;
    GuMessage queue[GU_MAX_MESSAGES];
    uint32_t queue_head, queue_count;
    int quit_posted;
    uint32_t quit_code;
    int32_t cursor_counter;
    uint32_t cursor;
    char last_message_box[GU_WINDOW_TEXT_MAX];
    uint32_t metrics_calls;
};

static void put32(uint8_t *b, uint32_t value) {
    b[0] = (uint8_t)value; b[1] = (uint8_t)(value >> 8);
    b[2] = (uint8_t)(value >> 16); b[3] = (uint8_t)(value >> 24);
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}
static int write_u32(GuestUser32 *u, uint32_t address, uint32_t value) {
    uint8_t b[4]; put32(b, value); return gm_write(u->memory, address, b, 4);
}
static int last_error(GuestUser32 *u, uint32_t value) {
    return write_u32(u, u->teb + 0x34, value);
}
/* ASCII only: a byte above 0x7f would be a lossy codepage conversion. */
static int read_ansi(GuestUser32 *u, uint32_t address, char *out, uint32_t capacity) {
    if (!capacity) return GU_STOP_COM_BAD_ARGS;
    for (uint32_t i = 0; i < capacity; i++) {
        uint8_t byte = 0;
        int rc = gm_read(u->memory, address + i, &byte, 1);
        if (rc) return rc;
        if (byte > 0x7f) return GU_STOP_NON_ASCII;
        out[i] = (char)byte;
        if (!byte) return 0;
    }
    return GU_STOP_STRING_TOO_LONG;
}
static int read_wide(GuestUser32 *u, uint32_t address, char *out, uint32_t capacity) {
    if (!capacity) return GU_STOP_COM_BAD_ARGS;
    for (uint32_t i = 0; i < capacity; i++) {
        uint8_t bytes[2];
        int rc = gm_read(u->memory, address + i * 2, bytes, 2);
        if (rc) return rc;
        uint32_t unit = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
        if (unit > 0x7f) return GU_STOP_NON_ASCII;
        out[i] = (char)unit;
        if (!unit) return 0;
    }
    return GU_STOP_STRING_TOO_LONG;
}
static int ascii_equal(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

GuestUser32 *gu_create(GuestMemory *memory, uint32_t teb) {
    if (!memory) return NULL;
    GuestUser32 *u = (GuestUser32 *)calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->memory = memory;
    u->teb = teb;
    if (pthread_mutex_init(&u->lock, NULL) != 0) { free(u); return NULL; }
    return u;
}
void gu_destroy(GuestUser32 *u) {
    if (!u) return;
    pthread_mutex_destroy(&u->lock);
    free(u);
}
int gu_set_surface_size(GuestUser32 *u, uint32_t width, uint32_t height) {
    if (!u || !width || !height) return GM_UNSUPPORTED;
    pthread_mutex_lock(&u->lock);
    u->surface_w = width; u->surface_h = height;
    if (u->window.used) {
        /* The host surface is the client area of the modelled window. */
        u->window.client_w = width; u->window.client_h = height;
        u->window.w = width; u->window.h = height;
    }
    pthread_mutex_unlock(&u->lock);
    return GM_OK;
}
int gu_set_guest_call(GuestUser32 *u, GuGuestCall call, void *context) {
    if (!u) return GM_UNSUPPORTED;
    u->call = call; u->call_context = context; return GM_OK;
}
int gu_set_message_wait(GuestUser32 *u, GuMessageWait wait, void *context) {
    if (!u) return GM_UNSUPPORTED;
    u->wait = wait; u->wait_context = context; return GM_OK;
}
int gu_set_scratch(GuestUser32 *u, uint32_t address, uint32_t size) {
    if (!u || !size) return GM_UNSUPPORTED;
    int rc = gm_check(u->memory, address, size, GM_WRITE);
    if (rc) return rc;
    u->scratch = address; u->scratch_size = size; return GM_OK;
}
uint32_t gu_message_count(GuestUser32 *u) {
    if (!u) return 0;
    pthread_mutex_lock(&u->lock);
    uint32_t n = u->queue_count;
    pthread_mutex_unlock(&u->lock);
    return n;
}
uint32_t gu_window_count(GuestUser32 *u) { return u && u->window.used ? 1u : 0u; }
uint32_t gu_hwnd(GuestUser32 *u) { return u && u->window.used ? u->window.hwnd : 0u; }
int gu_window_visible(GuestUser32 *u) { return u ? u->window.visible : 0; }
uint32_t gu_quit_code(GuestUser32 *u) { return u ? u->quit_code : 0u; }
int gu_quit_posted(GuestUser32 *u) { return u ? u->quit_posted : 0; }
int gu_cursor_counter(GuestUser32 *u) { return u ? u->cursor_counter : 0; }
uint32_t gu_cursor(GuestUser32 *u) { return u ? u->cursor : 0u; }
uint32_t gu_surface_width(GuestUser32 *u) { return u ? u->surface_w : 0u; }
uint32_t gu_surface_height(GuestUser32 *u) { return u ? u->surface_h : 0u; }
uint32_t gu_class_count(GuestUser32 *u) { return u ? u->class_count : 0u; }
uint32_t gu_metrics_calls(GuestUser32 *u) { return u ? u->metrics_calls : 0u; }
int gu_last_message_box(GuestUser32 *u, char *text, uint32_t capacity) {
    if (!u || !text || !capacity) return GM_UNSUPPORTED;
    size_t n = strlen(u->last_message_box);
    if (n >= capacity) n = capacity - 1;
    memcpy(text, u->last_message_box, n);
    text[n] = 0;
    return GM_OK;
}

/* --- message queue ------------------------------------------------------ */
/* Returns 0 when the bounded queue is full; the caller turns that into a
 * model stop instead of silently dropping a message. */
static int queue_push_locked(GuestUser32 *u, uint32_t hwnd, uint32_t message,
                             uint32_t wparam, uint32_t lparam) {
    if (u->queue_count == GU_MAX_MESSAGES) return 1;
    uint32_t slot = (u->queue_head + u->queue_count) % GU_MAX_MESSAGES;
    GuMessage *m = &u->queue[slot];
    m->hwnd = hwnd; m->message = message; m->wparam = wparam; m->lparam = lparam;
    m->time = 0; m->pt_x = 0; m->pt_y = 0;
    u->queue_count++;
    return 0;
}
static int queue_match(const GuestUser32 *u, uint32_t index, uint32_t hwnd,
                       uint32_t minimum, uint32_t maximum) {
    const GuMessage *m = &u->queue[(u->queue_head + index) % GU_MAX_MESSAGES];
    if (hwnd && m->hwnd != hwnd) return 0;
    if (minimum || maximum) {
        if (m->message < minimum) return 0;
        if (maximum && m->message > maximum) return 0;
    }
    return 1;
}
/* Finds a matching message, optionally removes it and copies it out. */
static int queue_take(GuestUser32 *u, uint32_t hwnd, uint32_t minimum,
                      uint32_t maximum, int remove, GuMessage *out) {
    int found = 0;
    pthread_mutex_lock(&u->lock);
    for (uint32_t i = 0; i < u->queue_count; i++) {
        if (!queue_match(u, i, hwnd, minimum, maximum)) continue;
        uint32_t slot = (u->queue_head + i) % GU_MAX_MESSAGES;
        *out = u->queue[slot];
        if (remove) {
            for (uint32_t j = i; j + 1 < u->queue_count; j++)
                u->queue[(u->queue_head + j) % GU_MAX_MESSAGES] =
                    u->queue[(u->queue_head + j + 1) % GU_MAX_MESSAGES];
            u->queue_count--;
        }
        found = 1;
        break;
    }
    pthread_mutex_unlock(&u->lock);
    return found;
}
static int write_message(GuestUser32 *u, uint32_t address, const GuMessage *m) {
    uint8_t bytes[28];
    put32(bytes + 0, m->hwnd); put32(bytes + 4, m->message);
    put32(bytes + 8, m->wparam); put32(bytes + 12, m->lparam);
    put32(bytes + 16, m->time); put32(bytes + 20, m->pt_x);
    put32(bytes + 24, m->pt_y);
    return gm_write(u->memory, address, bytes, sizeof(bytes));
}
static int read_message(GuestUser32 *u, uint32_t address, GuMessage *m) {
    uint8_t bytes[28];
    int rc = gm_read(u->memory, address, bytes, sizeof(bytes));
    if (rc) return rc;
    m->hwnd = get32(bytes + 0); m->message = get32(bytes + 4);
    m->wparam = get32(bytes + 8); m->lparam = get32(bytes + 12);
    m->time = get32(bytes + 16); m->pt_x = get32(bytes + 20);
    m->pt_y = get32(bytes + 24);
    return 0;
}

/* --- guest calls -------------------------------------------------------- */
/* Delivers one message to the window procedure. The hook owns the guest CPU;
 * without it the model stops instead of pretending the message was handled. */
static int send_message(GuestUser32 *u, uint32_t message, uint32_t wparam,
                        uint32_t lparam, uint32_t *result) {
    if (!u->call) return GU_STOP_NO_GUEST_CALL;
    uint32_t args[4];
    args[0] = u->window.hwnd; args[1] = message;
    args[2] = wparam; args[3] = lparam;
    uint32_t value = 0;
    int rc = u->call(u->call_context, u->window.wndproc, args, 4, &value);
    if (rc) return rc;
    *result = value;
    return 0;
}

/* --- window class registration ----------------------------------------- */
static int register_class(GuestUser32 *u, const uint32_t *args, uint32_t *result,
                          int wide) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint32_t cb_size = args[0];
    if (cb_size != 48) {
        int rc = last_error(u, ERROR_INVALID_PARAMETER_); if (rc) return rc;
        *result = 0; return 0;
    }
    char name[GU_CLASS_NAME_MAX];
    int rc = wide ? read_wide(u, args[10], name, sizeof(name))
                  : read_ansi(u, args[10], name, sizeof(name));
    if (rc) return rc;
    if (!name[0] || !args[2]) {
        rc = last_error(u, ERROR_INVALID_PARAMETER_); if (rc) return rc;
        *result = 0; return 0;
    }
    for (uint32_t i = 0; i < u->class_count; i++) {
        if (ascii_equal(u->classes[i].name, name)) {
            rc = last_error(u, ERROR_CLASS_ALREADY_EXISTS_); if (rc) return rc;
            *result = 0; return 0;
        }
    }
    if (u->class_count == GU_MAX_CLASSES) {
        rc = last_error(u, ERROR_NOT_ENOUGH_MEMORY_); if (rc) return rc;
        *result = 0; return 0;
    }
    GuWindowClass *c = &u->classes[u->class_count];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->atom = u->class_count + 1;
    c->style = args[1]; c->wndproc = args[2];
    c->cls_extra = args[3]; c->wnd_extra = args[4];
    c->instance = args[5]; c->icon = args[6]; c->cursor = args[7];
    c->background = args[8]; c->menu_name = args[9]; c->icon_small = args[11];
    memcpy(c->name, name, sizeof(c->name));
    u->class_count++;
    *result = c->atom;
    return 0;
}
/* RegisterClassEx takes ONE argument: a pointer to WNDCLASSEX (48 bytes). The
 * fields are read from guest memory and handed to register_class() as words.
 * (They used to be read from the stack as if the struct were passed by value,
 * which also popped 48 instead of 4 bytes.) */
static int register_class_ptr(GuestUser32 *u, uint32_t address, uint32_t *result, int wide, int ex) {
    if (!u || !result) return GU_STOP_COM_BAD_ARGS;
    uint8_t raw[48];
    uint32_t w[12];
    memset(raw, 0, sizeof raw);
    int rc = gm_read(u->memory, address, raw, ex ? 48u : 40u);
    if (rc) return rc;
    if (ex) for (unsigned i = 0; i < 12; i++) w[i] = get32(raw + i * 4);
    else {                                          /* WNDCLASS = WNDCLASSEX without cbSize / hIconSm */
        w[0] = 48;
        for (unsigned i = 0; i < 10; i++) w[i + 1] = get32(raw + i * 4);
        w[11] = 0;
    }
    return register_class(u, w, result, wide);
}
int gu_register_class_ex_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return register_class_ptr(u, args[0], result, 0, 1);
}
int gu_register_class_ex_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return register_class_ptr(u, args[0], result, 1, 1);
}
/* GetClassInfoExA(hInstance, lpszClass, lpwcx): only classes registered by the
 * guest exist (no system classes are modelled). lpszClassName/lpszMenuName in
 * the result are the caller's own pointers, as on Windows for app classes. */
int gu_get_class_info_ex_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    char name[GU_CLASS_NAME_MAX];
    int rc;
    if (args[1] < 0x10000u) {                       /* atom */
        name[0] = 0;
        for (uint32_t i = 0; i < u->class_count; i++)
            if (u->classes[i].atom == args[1]) memcpy(name, u->classes[i].name, sizeof name);
    } else if ((rc = read_ansi(u, args[1], name, sizeof(name)))) return rc;
    for (uint32_t i = 0; name[0] && i < u->class_count; i++) {
        const GuWindowClass *c = &u->classes[i];
        if (!ascii_equal(c->name, name)) continue;
        uint8_t raw[48];
        uint32_t w[12] = { 48, c->style, c->wndproc, c->cls_extra, c->wnd_extra, c->instance,
                           c->icon, c->cursor, c->background, c->menu_name, args[1], c->icon_small };
        for (unsigned k = 0; k < 12; k++) put32(raw + k * 4, w[k]);
        if ((rc = gm_write(u->memory, args[2], raw, sizeof raw))) return rc;
        *result = c->atom;
        return 0;
    }
    rc = last_error(u, 1411);                       /* ERROR_CLASS_DOES_NOT_EXIST */
    if (rc) return rc;
    *result = 0;
    return 0;
}
int gu_register_class_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return register_class_ptr(u, args[0], result, 0, 0);
}
int gu_register_class_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return register_class_ptr(u, args[0], result, 1, 0);
}

/* --- window creation ---------------------------------------------------- */
static int find_class(GuestUser32 *u, uint32_t name_or_atom, int wide, int *index) {
    char name[GU_CLASS_NAME_MAX];
    /* A class name is either an atom (high word 0, like MAKEINTRESOURCE) or a
     * guest pointer. Guest addresses always have a non-zero high word. */
    if (name_or_atom && (name_or_atom & 0xffff0000u) == 0) {
        for (uint32_t i = 0; i < u->class_count; i++)
            if (u->classes[i].atom == name_or_atom) { *index = (int)i; return 0; }
        return 1;
    }
    if (!name_or_atom) return 1;
    int rc = wide ? read_wide(u, name_or_atom, name, sizeof(name))
                  : read_ansi(u, name_or_atom, name, sizeof(name));
    if (rc) return rc;
    for (uint32_t i = 0; i < u->class_count; i++)
        if (ascii_equal(u->classes[i].name, name)) { *index = (int)i; return 0; }
    return 1;
}
/* CREATESTRUCTA (48 bytes) in the optional guest scratch block. */
static uint32_t write_create_struct(GuestUser32 *u, uint32_t instance,
                                    uint32_t menu, uint32_t parent,
                                    uint32_t height, uint32_t width,
                                    uint32_t y, uint32_t x, uint32_t style,
                                    uint32_t exstyle, uint32_t class_name,
                                    uint32_t title, uint32_t param) {
    if (!u->scratch || u->scratch_size < 48) return 0;
    uint8_t bytes[48];
    put32(bytes + 0, param); put32(bytes + 4, instance);
    put32(bytes + 8, menu); put32(bytes + 12, parent);
    put32(bytes + 16, height); put32(bytes + 20, width);
    put32(bytes + 24, y); put32(bytes + 28, x);
    put32(bytes + 32, style); put32(bytes + 36, title);
    put32(bytes + 40, class_name); put32(bytes + 44, exstyle);
    if (gm_write(u->memory, u->scratch, bytes, sizeof(bytes))) return 0;
    return u->scratch;
}
static int create_window(GuestUser32 *u, const uint32_t *args, uint32_t *result,
                         int wide) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint32_t exstyle = args[0], class_name = args[1], title = args[2];
    uint32_t style = args[3];
    uint32_t x = args[4], y = args[5], w = args[6], h = args[7];
    uint32_t parent = args[8], menu = args[9], instance = args[10];
    uint32_t param = args[11];
    if (parent || (style & WS_CHILD_)) return GU_STOP_CHILD_WINDOW;
    if (u->window.used) return GU_STOP_SECOND_WINDOW;
    if (!u->surface_w || !u->surface_h) return GU_STOP_NO_SURFACE;
    if (!u->call) return GU_STOP_NO_GUEST_CALL;
    int index = 0;
    int found = find_class(u, class_name, wide, &index);
    if (found != 0 && found != 1) return found;
    if (found) {
        int rc = last_error(u, ERROR_CANNOT_FIND_WND_CLASS_); if (rc) return rc;
        *result = 0; return 0;
    }
    char text[GU_WINDOW_TEXT_MAX];
    int rc = 0;
    if (title) {
        rc = wide ? read_wide(u, title, text, sizeof(text))
                  : read_ansi(u, title, text, sizeof(text));
        if (rc) return rc;
    } else text[0] = 0;
    if (x == CW_USEDEFAULT_) x = 0;
    if (y == CW_USEDEFAULT_) y = 0;
    if (w == CW_USEDEFAULT_ || w == 0) w = u->surface_w;
    if (h == CW_USEDEFAULT_ || h == 0) h = u->surface_h;

    GuWindow *win = &u->window;
    memset(win, 0, sizeof(*win));
    win->used = 1;
    win->hwnd = GU_HWND_MAIN;
    win->class_index = index;
    win->style = style; win->exstyle = exstyle;
    win->instance = instance; win->menu = menu;
    win->wndproc = u->classes[index].wndproc;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->client_w = w; win->client_h = h;
    memcpy(win->title, text, sizeof(win->title));

    uint32_t create_struct = write_create_struct(u, instance, menu, parent, h, w,
                                                 y, x, style, exstyle,
                                                 class_name, title, param);
    uint32_t value = 0;
    rc = send_message(u, WM_NCCREATE_, 0, create_struct, &value);
    if (rc) { win->used = 0; return rc; }
    if (!value) { win->used = 0; *result = 0; return 0; }
    rc = send_message(u, WM_CREATE_, 0, create_struct, &value);
    if (rc) { win->used = 0; return rc; }
    if (value == UINT32_MAX) { win->used = 0; *result = 0; return 0; }
    *result = win->hwnd;
    if (style & WS_VISIBLE_) {
        /* A window created visible is shown and activated by CreateWindowEx
         * itself (WM_SHOWWINDOW, WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS, WM_SIZE). */
        win->visible = 1;
        rc = send_message(u, WM_SHOWWINDOW_, 1, 0, &value);
        if (rc) return rc;
        if (!win->active) {
            win->active = 1;
            rc = send_message(u, WM_ACTIVATEAPP_, 1, 0, &value);
            if (rc) return rc;
        }
        rc = send_message(u, WM_ACTIVATE_, 1, 0, &value);
        if (rc) return rc;
        rc = send_message(u, WM_SETFOCUS_, 0, 0, &value);
        if (rc) return rc;
        rc = send_message(u, WM_SIZE_, 0, (win->client_h << 16) | (win->client_w & 0xffff), &value);
        if (rc) return rc;
    }
    return 0;
}
int gu_create_window_ex_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return create_window(u, args, result, 0);
}
int gu_create_window_ex_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return create_window(u, args, result, 1);
}

/* --- window state ------------------------------------------------------- */
/* 0 = modelled window, 1 = invalid handle (*result = 0, LastError set),
 * GM_* = guest memory stop. */
static int check_window(GuestUser32 *u, uint32_t hwnd, uint32_t *result) {
    if (u->window.used && hwnd == u->window.hwnd) return 0;
    int rc = last_error(u, ERROR_INVALID_WINDOW_HANDLE_); if (rc) return rc;
    *result = 0;
    return 1;
}
#define GU_WINDOW_CHECK(u, hwnd, result) do { \
        int gu_rc_ = check_window((u), (hwnd), (result)); \
        if (gu_rc_) return gu_rc_ == 1 ? 0 : gu_rc_; \
    } while (0)
static int destroy_window(GuestUser32 *u) {
    uint32_t value = 0;
    if (!u->window.used) return 0;
    int rc = send_message(u, WM_DESTROY_, 0, 0, &value);
    if (rc) return rc;
    rc = send_message(u, WM_NCDESTROY_, 0, 0, &value);
    if (rc) return rc;
    u->window.used = 0;
    u->window.visible = 0;
    return 0;
}
int gu_show_window(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint32_t hwnd = args[0], command = args[1];
    int previous = u->window.visible;
    GU_WINDOW_CHECK(u, hwnd, result);
    int show = command != 0; /* SW_HIDE == 0, everything else shows */
    u->window.visible = show;
    uint32_t value = 0;
    int rc = send_message(u, WM_SHOWWINDOW_, show ? 1u : 0u, 0, &value);
    if (rc) return rc;
    if (show) {
        rc = send_message(u, WM_SIZE_, 0,
                          (u->window.client_h << 16) | (u->window.client_w & 0xffff),
                          &value);
        if (rc) return rc;
        /* Showing the only top-level window of the process activates the
         * application: WM_ACTIVATEAPP(TRUE) precedes WM_ACTIVATE, exactly once
         * per activation (games gate audio and simulation on it). */
        if (!u->window.active) {
            u->window.active = 1;
            rc = send_message(u, WM_ACTIVATEAPP_, 1, 0, &value);
            if (rc) return rc;
        }
        rc = send_message(u, WM_ACTIVATE_, 1, 0, &value);
        if (rc) return rc;
        rc = send_message(u, WM_SETFOCUS_, 0, 0, &value);
        if (rc) return rc;
    } else {
        rc = send_message(u, WM_KILLFOCUS_, 0, 0, &value);
        if (rc) return rc;
    }
    *result = previous ? 1u : 0u;
    return 0;
}
int gu_update_window(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint32_t hwnd = args[0];
    GU_WINDOW_CHECK(u, hwnd, result);
    /* No update-region tracking: UpdateWindow delivers WM_PAINT directly when
     * the window is visible (documented deviation). */
    if (u->window.visible) {
        uint32_t value = 0;
        int rc = send_message(u, WM_PAINT_, 0, 0, &value);
        if (rc) return rc;
    }
    *result = 1;
    return 0;
}
static int write_rect(GuestUser32 *u, uint32_t address, uint32_t left,
                      uint32_t top, uint32_t right, uint32_t bottom) {
    uint8_t bytes[16];
    put32(bytes + 0, left); put32(bytes + 4, top);
    put32(bytes + 8, right); put32(bytes + 12, bottom);
    return gm_write(u->memory, address, bytes, sizeof(bytes));
}
int gu_get_client_rect(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    GU_WINDOW_CHECK(u, args[0], result);
    int rc = write_rect(u, args[1], 0, 0, u->window.client_w, u->window.client_h);
    if (rc) return rc;
    *result = 1;
    return 0;
}
int gu_get_window_rect(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    GU_WINDOW_CHECK(u, args[0], result);
    int rc = write_rect(u, args[1], u->window.x, u->window.y,
                        u->window.x + u->window.w, u->window.y + u->window.h);
    if (rc) return rc;
    *result = 1;
    return 0;
}
static int adjust_rect(GuestUser32 *u, uint32_t address, uint32_t style,
                       uint32_t exstyle, uint32_t menu, uint32_t *result) {
    uint8_t bytes[16];
    int rc = gm_read(u->memory, address, bytes, sizeof(bytes));
    if (rc) return rc;
    uint32_t left = get32(bytes + 0), top = get32(bytes + 4);
    uint32_t right = get32(bytes + 8), bottom = get32(bytes + 12);
    if (!(style & WS_POPUP_)) {
        uint32_t border = 0;
        if (style & (WS_BORDER_ | WS_DLGFRAME_ | WS_THICKFRAME_)) border = GU_FRAME_BORDER;
        if (exstyle & (WS_EX_WINDOWEDGE_ | WS_EX_CLIENTEDGE_)) border++;
        left -= border; top -= border; right += border; bottom += border;
        if (style & WS_CAPTION_) top -= GU_FRAME_CAPTION;
        if (menu) top -= GU_FRAME_MENU;
    }
    rc = write_rect(u, address, left, top, right, bottom);
    if (rc) return rc;
    *result = 1;
    return 0;
}
int gu_adjust_window_rect(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    return adjust_rect(u, args[0], args[1], 0, args[2], result);
}
int gu_adjust_window_rect_ex(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    return adjust_rect(u, args[0], args[1], args[3], args[2], result);
}
static int window_long(GuestUser32 *u, const uint32_t *args, uint32_t *result,
                       int set) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    GU_WINDOW_CHECK(u, args[0], result);
    uint32_t index = args[1], value = args[2];
    uint32_t *slot = NULL;
    switch (index) {
    case GWL_STYLE_: slot = &u->window.style; break;
    case GWL_EXSTYLE_: slot = &u->window.exstyle; break;
    case GWL_WNDPROC_: slot = &u->window.wndproc; break;
    case GWL_HINSTANCE_: slot = &u->window.instance; break;
    case GWL_HWNDPARENT_: slot = &u->window.menu; break;
    case GWL_ID_: slot = &u->window.menu; break;
    case GWL_USERDATA_: slot = &u->window.userdata; break;
    default: {
        int rc = last_error(u, ERROR_INVALID_PARAMETER_); if (rc) return rc;
        *result = 0; return 0;
    }
    }
    uint32_t previous = *slot;
    if (set) *slot = value;
    *result = previous;
    return 0;
}
int gu_set_window_long_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return window_long(u, args, result, 1);
}
int gu_set_window_long_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return window_long(u, args, result, 1);
}
int gu_get_window_long_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return window_long(u, args, result, 0);
}
int gu_get_window_long_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return window_long(u, args, result, 0);
}

/* --- metrics and display modes ----------------------------------------- */
int gu_get_system_metrics(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    if (!u->surface_w || !u->surface_h) return GU_STOP_NO_SURFACE;
    uint32_t w = u->surface_w, h = u->surface_h;
    u->metrics_calls++;
    switch (args[0]) {
    case 0: *result = w; return 0;                 /* SM_CXSCREEN */
    case 1: *result = h; return 0;                 /* SM_CYSCREEN */
    case 2: *result = GU_FRAME_BORDER; return 0;   /* SM_CXVSCROLL */
    case 3: *result = GU_FRAME_BORDER; return 0;   /* SM_CYHSCROLL */
    case 4: *result = GU_FRAME_CAPTION; return 0;  /* SM_CYCAPTION */
    case 5: *result = 1; return 0;                 /* SM_CXBORDER */
    case 6: *result = 1; return 0;                 /* SM_CYBORDER */
    case 7: *result = GU_FRAME_BORDER; return 0;   /* SM_CXDLGFRAME */
    case 8: *result = GU_FRAME_BORDER; return 0;   /* SM_CYDLGFRAME */
    case 11: *result = GU_SM_CXICON; return 0;     /* SM_CXICON */
    case 12: *result = GU_SM_CYICON; return 0;     /* SM_CYICON */
    case 13: *result = GU_SM_CXCURSOR; return 0;   /* SM_CXCURSOR */
    case 14: *result = GU_SM_CYCURSOR; return 0;   /* SM_CYCURSOR */
    case 15: *result = GU_FRAME_MENU; return 0;    /* SM_CYMENU */
    case 16: *result = w; return 0;                /* SM_CXFULLSCREEN */
    case 17: *result = h; return 0;                /* SM_CYFULLSCREEN */
    case 19: *result = 1; return 0;                /* SM_MOUSEPRESENT */
    case 20: *result = 4; return 0;                /* SM_CYVSCROLL */
    case 21: *result = 17; return 0;               /* SM_CXHSCROLL */
    case 22: *result = 17; return 0;               /* SM_CYHSCROLL */
    case 23: *result = 0; return 0;                /* SM_SWAPBUTTON */
    case 28: *result = 112; return 0;              /* SM_CXMIN */
    case 29: *result = 27; return 0;               /* SM_CYMIN */
    case 30: *result = GU_FRAME_CAPTION; return 0; /* SM_CXSIZE */
    case 31: *result = GU_FRAME_CAPTION; return 0; /* SM_CYSIZE */
    case 32: *result = GU_FRAME_BORDER; return 0;  /* SM_CXFRAME */
    case 33: *result = GU_FRAME_BORDER; return 0;  /* SM_CYFRAME */
    case 36: *result = 4; return 0;                /* SM_CXDOUBLECLK */
    case 37: *result = 4; return 0;                /* SM_CYDOUBLECLK */
    case 45: *result = 2; return 0;                /* SM_CXEDGE */
    case 46: *result = 2; return 0;                /* SM_CYEDGE */
    case 49: *result = GU_SM_CXSMICON; return 0;   /* SM_CXSMICON */
    case 50: *result = GU_SM_CYSMICON; return 0;   /* SM_CYSMICON */
    case 59: *result = w; return 0;                /* SM_CXMAXTRACK */
    case 60: *result = h; return 0;                /* SM_CYMAXTRACK */
    case 61: *result = w; return 0;                /* SM_CXMAXIMIZED */
    case 62: *result = h; return 0;                /* SM_CYMAXIMIZED */
    case 67: *result = 0; return 0;                /* SM_CLEANBOOT */
    case 68: *result = 4; return 0;                /* SM_CXDRAG */
    case 69: *result = 4; return 0;                /* SM_CYDRAG */
    case 75: *result = 1; return 0;                /* SM_MOUSEWHEELPRESENT */
    case 78: *result = 0; return 0;                /* SM_XVIRTUALSCREEN */
    case 79: *result = 0; return 0;                /* SM_YVIRTUALSCREEN */
    case 80: *result = 1; return 0;                /* SM_CMONITORS */
    case 81: *result = w; return 0;                /* SM_CXVIRTUALSCREEN */
    case 82: *result = h; return 0;                /* SM_CYVIRTUALSCREEN */
    case 0x1000: *result = 0; return 0;            /* SM_REMOTESESSION */
    default:
        /* Documented Win32 behaviour for an unknown index is 0 ("metric not
         * present"). It is not a fabricated success: the counter is exposed
         * for diagnostics and no capability is claimed. */
        *result = 0;
        return 0;
    }
}
/* DEVMODEA is 156 bytes, DEVMODEW 220 bytes (PE32, dmDeviceName/dmFormName
 * are TCHAR[32]). */
static int enum_display(GuestUser32 *u, const uint32_t *args, uint32_t *result,
                        int wide) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    if (!u->surface_w || !u->surface_h) return GU_STOP_NO_SURFACE;
    uint32_t mode = args[1], devmode = args[2];
    uint32_t size = wide ? 220u : 156u;
    if (!devmode) {
        int rc = last_error(u, ERROR_INVALID_PARAMETER_); if (rc) return rc;
        *result = 0; return 0;
    }
    uint8_t header[4];
    int rc = gm_read(u->memory, devmode + (wide ? 68u : 36u), header, 4);
    if (rc) return rc;
    uint16_t dm_size = (uint16_t)(header[0] | (header[1] << 8));
    if (dm_size && dm_size != size) {
        rc = last_error(u, ERROR_INVALID_PARAMETER_); if (rc) return rc;
        *result = 0; return 0;
    }
    /* One mode: the host surface at 60 Hz, 32 bpp. Mode 0 and the two
     * ENUM_* sentinels select it; any higher index means "no more modes". */
    if (mode != 0 && mode != ENUM_CURRENT_SETTINGS_ && mode != ENUM_REGISTRY_SETTINGS_) {
        *result = 0;
        return 0;
    }
    uint8_t bytes[220];
    memset(bytes, 0, sizeof(bytes));
    static const char name[] = "\\\\.\\DISPLAY1";
    if (wide) {
        for (uint32_t i = 0; name[i]; i++) {
            bytes[i * 2] = (uint8_t)name[i];
        }
    } else {
        memcpy(bytes, name, sizeof(name));
    }
    /* DM: dmDeviceName[32] is 32 bytes ANSI / 64 bytes wide, so
     * dmSpecVersion sits at 32/64 and dmSize at 36/68. */
    uint32_t spec = wide ? 64u : 32u, fields = wide ? 72u : 40u;
    bytes[spec + 0] = 0x01; bytes[spec + 1] = 0x04;      /* dmSpecVersion 0x0401 */
    bytes[spec + 2] = 0x00; bytes[spec + 3] = 0x00;      /* dmDriverVersion */
    bytes[spec + 4] = (uint8_t)(size & 0xff); bytes[spec + 5] = (uint8_t)(size >> 8);
    /* dmFields: DM_BITSPERPEL|DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY */
    put32(bytes + fields, 0x00400000u | 0x00080000u | 0x00100000u | 0x00040000u);
    /* DEVMODEA: dmBitsPerPel 104, dmPelsWidth 108, dmPelsHeight 112,
     * dmDisplayFlags 116, dmDisplayFrequency 120.
     * DEVMODEW: 168/172/176/180/184 (both TCHAR[32] fields are 64 bytes). */
    uint32_t bpp = wide ? 168u : 104u;
    uint32_t width = wide ? 172u : 108u;
    uint32_t height = wide ? 176u : 112u;
    uint32_t flags = wide ? 180u : 116u;
    uint32_t freq = wide ? 184u : 120u;
    put32(bytes + bpp, 32);
    put32(bytes + width, u->surface_w);
    put32(bytes + height, u->surface_h);
    put32(bytes + flags, 0);
    put32(bytes + freq, 60);
    rc = gm_write(u->memory, devmode, bytes, size);
    if (rc) return rc;
    *result = 1;
    return 0;
}
int gu_enum_display_settings_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return enum_display(u, args, result, 0);
}
int gu_enum_display_settings_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return enum_display(u, args, result, 1);
}

/* --- cursors, icons, message boxes ------------------------------------- */
int gu_show_cursor(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    u->cursor_counter += args[0] ? 1 : -1;
    *result = (uint32_t)u->cursor_counter;
    return 0;
}
int gu_set_cursor(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint32_t previous = u->cursor;
    u->cursor = args[0];
    *result = previous;
    return 0;
}
/* Predefined cursor/icon identifiers are MAKEINTRESOURCE values (<= 0xffff)
 * and are returned unchanged; a named resource would need a resource tree. */
static int load_resource(GuestUser32 *u, const uint32_t *args, uint32_t *result,
                         int stop) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    if (!args[1]) {
        *result = 0;
        return 0;
    }
    if ((args[1] & 0xffff0000u) == 0) { *result = args[1]; return 0; }
    return stop;
}
int gu_load_cursor_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return load_resource(u, args, result, GU_STOP_CURSOR_RESOURCE);
}
int gu_load_cursor_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return load_resource(u, args, result, GU_STOP_CURSOR_RESOURCE);
}
int gu_load_icon_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return load_resource(u, args, result, GU_STOP_ICON_RESOURCE);
}
int gu_load_icon_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return load_resource(u, args, result, GU_STOP_ICON_RESOURCE);
}
static int message_box(GuestUser32 *u, const uint32_t *args, uint32_t *result,
                       int wide) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    char text[GU_WINDOW_TEXT_MAX], caption[GU_WINDOW_TEXT_MAX];
    int rc = 0;
    if (args[1]) rc = wide ? read_wide(u, args[1], text, sizeof(text))
                           : read_ansi(u, args[1], text, sizeof(text));
    else text[0] = 0;
    if (rc) return rc;
    if (args[2]) rc = wide ? read_wide(u, args[2], caption, sizeof(caption))
                           : read_ansi(u, args[2], caption, sizeof(caption));
    else caption[0] = 0;
    if (rc) return rc;
    snprintf(u->last_message_box, sizeof(u->last_message_box), "%s", text);
    fprintf(stderr, "user32-messagebox: type=%#x caption=\"%s\" text=\"%s\"\n",
            args[3], caption, text);
    *result = 1; /* IDOK */
    return 0;
}
int gu_message_box_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return message_box(u, args, result, 0);
}
int gu_message_box_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return message_box(u, args, result, 1);
}

/* --- message loop ------------------------------------------------------- */
int gu_post_quit_message(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    pthread_mutex_lock(&u->lock);
    u->quit_posted = 1;
    u->quit_code = args[0];
    int full = queue_push_locked(u, 0, WM_QUIT_, args[0], 0);
    pthread_mutex_unlock(&u->lock);
    if (full) return GM_UNSUPPORTED;
    *result = 0;
    return 0;
}
static int peek_or_get(GuestUser32 *u, const uint32_t *args, uint32_t *result,
                       int remove, int blocking) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint32_t msg = args[0], hwnd = args[1];
    uint32_t minimum = args[2], maximum = args[3];
    GuMessage message;
    for (;;) {
        if (queue_take(u, hwnd, minimum, maximum, remove, &message)) {
            int rc = write_message(u, msg, &message);
            if (rc) return rc;
            *result = message.message == WM_QUIT_ ? 0u : 1u;
            return 0;
        }
        if (!blocking) { *result = 0; return 0; }
        if (!u->wait) return GU_STOP_MESSAGE_WAIT;
        int rc = u->wait(u->wait_context, 0);
        if (rc < 0) return rc;
        if (rc == 0) {
            /* The host reports shutdown: synthesise WM_QUIT exactly once. */
            pthread_mutex_lock(&u->lock);
            if (!u->quit_posted) {
                u->quit_posted = 1;
                (void)queue_push_locked(u, 0, WM_QUIT_, u->quit_code, 0);
            }
            pthread_mutex_unlock(&u->lock);
        }
    }
}
int gu_peek_message_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return peek_or_get(u, args, result, (args && (args[4] & 1)) ? 1 : 0, 0);
}
int gu_peek_message_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return peek_or_get(u, args, result, (args && (args[4] & 1)) ? 1 : 0, 0);
}
int gu_get_message_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return peek_or_get(u, args, result, 1, 1);
}
int gu_get_message_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return peek_or_get(u, args, result, 1, 1);
}
/* Virtual key -> ASCII for the printable range only (no keyboard layout, no
 * dead keys, no IME). TranslateMessage returns 0 for everything else. */
static int key_to_char(uint32_t vk, uint32_t *character) {
    if (vk >= 'A' && vk <= 'Z') { *character = vk; return 1; }
    if (vk >= '0' && vk <= '9') { *character = vk; return 1; }
    if (vk >= 0x60 && vk <= 0x69) { *character = vk - 0x60 + '0'; return 1; }
    switch (vk) {
    case 0x20: *character = ' '; return 1;
    case 0x0d: *character = '\r'; return 1;
    case 0x09: *character = '\t'; return 1;
    case 0x08: *character = 0x08; return 1;
    case 0x1b: *character = 0x1b; return 1;
    case 0xba: *character = ';'; return 1;
    case 0xbb: *character = '='; return 1;
    case 0xbc: *character = ','; return 1;
    case 0xbd: *character = '-'; return 1;
    case 0xbe: *character = '.'; return 1;
    case 0xbf: *character = '/'; return 1;
    case 0xc0: *character = '`'; return 1;
    case 0xdb: *character = '['; return 1;
    case 0xdc: *character = '\\'; return 1;
    case 0xdd: *character = ']'; return 1;
    case 0xde: *character = '\''; return 1;
    default: return 0;
    }
}
int gu_translate_message(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    GuMessage message;
    int rc = read_message(u, args[0], &message);
    if (rc) return rc;
    uint32_t character = 0;
    if (message.message == WM_KEYDOWN_ || message.message == WM_SYSKEYDOWN_) {
        if (key_to_char(message.wparam, &character)) {
            uint32_t translated = message.message == WM_SYSKEYDOWN_
                                      ? WM_SYSCHAR_ : WM_CHAR_;
            pthread_mutex_lock(&u->lock);
            int full = queue_push_locked(u, message.hwnd, translated, character,
                                         message.lparam);
            pthread_mutex_unlock(&u->lock);
            if (full) return GM_UNSUPPORTED;
            *result = 1;
            return 0;
        }
    }
    *result = 0;
    return 0;
}
int gu_dispatch_message_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    GuMessage message;
    int rc = read_message(u, args[0], &message);
    if (rc) return rc;
    if (!message.hwnd) { *result = 0; return 0; } /* thread message */
    if (!u->window.used || message.hwnd != u->window.hwnd) {
        int lasterror = last_error(u, ERROR_INVALID_WINDOW_HANDLE_);
        if (lasterror) return lasterror;
        *result = 0;
        return 0;
    }
    if (!u->call) return GU_STOP_NO_GUEST_CALL;
    uint32_t call_args[4];
    call_args[0] = message.hwnd; call_args[1] = message.message;
    call_args[2] = message.wparam; call_args[3] = message.lparam;
    uint32_t value = 0;
    rc = u->call(u->call_context, u->window.wndproc, call_args, 4, &value);
    if (rc) return rc;
    *result = value;
    return 0;
}
int gu_dispatch_message_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return gu_dispatch_message_a(u, args, result);
}

/* --- DefWindowProc ------------------------------------------------------ */
static int def_window_proc(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    if (!u || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint32_t hwnd = args[0], message = args[1];
    uint32_t wparam = args[2];
    if (!u->window.used || hwnd != u->window.hwnd) return GU_STOP_UNKNOWN_WINDOW;
    switch (message) {
    case WM_NCCREATE_: *result = 1; return 0;
    case WM_CREATE_: *result = 0; return 0;
    case WM_NCCALCSIZE_: *result = 0; return 0;
    case WM_NCHITTEST_: *result = 1; return 0;   /* HTCLIENT */
    case WM_GETMINMAXINFO_: *result = 0; return 0;
    case WM_ERASEBKGND_: *result = 1; return 0;
    case WM_PAINT_: *result = 0; return 0;
    case WM_SETCURSOR_: *result = 1; return 0;
    case WM_ACTIVATE_:
    case WM_ACTIVATEAPP_:
    case WM_SETFOCUS_:
    case WM_KILLFOCUS_:
    case WM_SIZE_:
    case WM_MOVE_:
    case WM_DISPLAYCHANGE_:
    case WM_SHOWWINDOW_: *result = 0; return 0;
    case WM_CLOSE_: {
        int rc = destroy_window(u);
        if (rc) return rc;
        *result = 0;
        return 0;
    }
    case WM_SYSCOMMAND_:
        if ((wparam & 0xfff0u) == 0xf060u) { /* SC_CLOSE */
            int rc = destroy_window(u);
            if (rc) return rc;
        }
        *result = 0;
        return 0;
    case WM_DESTROY_:
    case WM_NCDESTROY_: *result = 0; return 0;
    default: *result = 0; return 0;
    }
}
int gu_def_window_proc_a(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return def_window_proc(u, args, result);
}
int gu_def_window_proc_w(GuestUser32 *u, const uint32_t *args, uint32_t *result) {
    return def_window_proc(u, args, result);
}

/* --- host input --------------------------------------------------------- */
static uint32_t make_lparam(uint32_t low, uint32_t high) {
    return (low & 0xffffu) | ((high & 0xffffu) << 16);
}
int gu_post_host_input(GuestUser32 *u, const GuHostInput *input) {
    if (!u || !input) return GU_STOP_COM_BAD_ARGS;
    int full = 0;
    pthread_mutex_lock(&u->lock);
    uint32_t hwnd = u->window.used ? u->window.hwnd : 0;
    switch (input->type) {
    case GU_INPUT_TOUCH_DOWN:
        full |= queue_push_locked(u, hwnd, WM_MOUSEMOVE_, 0,
                                  make_lparam(input->x, input->y));
        full |= queue_push_locked(u, hwnd, WM_LBUTTONDOWN_, 1,
                                  make_lparam(input->x, input->y));
        break;
    case GU_INPUT_TOUCH_UP:
        full |= queue_push_locked(u, hwnd, WM_LBUTTONUP_, 0,
                                  make_lparam(input->x, input->y));
        break;
    case GU_INPUT_TOUCH_MOVE:
        full |= queue_push_locked(u, hwnd, WM_MOUSEMOVE_, 0,
                                  make_lparam(input->x, input->y));
        break;
    case GU_INPUT_KEY_DOWN:
        full |= queue_push_locked(u, hwnd, WM_KEYDOWN_, input->code,
                                  1u | (input->code << 16));
        break;
    case GU_INPUT_KEY_UP:
        full |= queue_push_locked(u, hwnd, WM_KEYUP_, input->code,
                                  0xc0000001u | (input->code << 16));
        break;
    case GU_INPUT_RESIZE:
        if (input->width && input->height) {
            u->surface_w = input->width;
            u->surface_h = input->height;
            if (u->window.used) {
                u->window.client_w = input->width;
                u->window.client_h = input->height;
                u->window.w = input->width;
                u->window.h = input->height;
            }
            full |= queue_push_locked(u, hwnd, WM_SIZE_, 0,
                                      make_lparam(input->width, input->height));
            full |= queue_push_locked(u, hwnd, WM_DISPLAYCHANGE_, 32,
                                      make_lparam(input->width, input->height));
        }
        break;
    case GU_INPUT_CLOSE:
        full |= queue_push_locked(u, hwnd, WM_CLOSE_, 0, 0);
        break;
    case GU_INPUT_FOCUS_GAINED:
        if (!u->window.active) { u->window.active = 1; full |= queue_push_locked(u, hwnd, WM_ACTIVATEAPP_, 1, 0); }
        full |= queue_push_locked(u, hwnd, WM_ACTIVATE_, 1, 0);
        full |= queue_push_locked(u, hwnd, WM_SETFOCUS_, 0, 0);
        break;
    case GU_INPUT_FOCUS_LOST:
        full |= queue_push_locked(u, hwnd, WM_KILLFOCUS_, 0, 0);
        full |= queue_push_locked(u, hwnd, WM_ACTIVATE_, 0, 0);
        if (u->window.active) { u->window.active = 0; full |= queue_push_locked(u, hwnd, WM_ACTIVATEAPP_, 0, 0); }
        break;
    default:
        pthread_mutex_unlock(&u->lock);
        return GU_STOP_COM_BAD_ARGS;
    }
    pthread_mutex_unlock(&u->lock);
    return full ? GM_UNSUPPORTED : GM_OK;
}

/* --- registration list -------------------------------------------------- */
#define GU_ARGS(n) ((uint32_t)((n) * 4u))
const GuUser32Export gu_user32_exports[] = {
    { "user32.dll", "RegisterClassExA", GU_ARGS(1), gu_register_class_ex_a },
    { "user32.dll", "RegisterClassExW", GU_ARGS(1), gu_register_class_ex_w },
    { "user32.dll", "RegisterClassA", GU_ARGS(1), gu_register_class_a },
    { "user32.dll", "GetClassInfoExA", GU_ARGS(3), gu_get_class_info_ex_a },
    { "user32.dll", "RegisterClassW", GU_ARGS(1), gu_register_class_w },
    { "user32.dll", "CreateWindowExA", GU_ARGS(12), gu_create_window_ex_a },
    { "user32.dll", "CreateWindowExW", GU_ARGS(12), gu_create_window_ex_w },
    { "user32.dll", "DefWindowProcA", GU_ARGS(4), gu_def_window_proc_a },
    { "user32.dll", "DefWindowProcW", GU_ARGS(4), gu_def_window_proc_w },
    { "user32.dll", "PeekMessageA", GU_ARGS(5), gu_peek_message_a },
    { "user32.dll", "PeekMessageW", GU_ARGS(5), gu_peek_message_w },
    { "user32.dll", "GetMessageA", GU_ARGS(4), gu_get_message_a },
    { "user32.dll", "GetMessageW", GU_ARGS(4), gu_get_message_w },
    { "user32.dll", "TranslateMessage", GU_ARGS(1), gu_translate_message },
    { "user32.dll", "DispatchMessageA", GU_ARGS(1), gu_dispatch_message_a },
    { "user32.dll", "DispatchMessageW", GU_ARGS(1), gu_dispatch_message_w },
    { "user32.dll", "PostQuitMessage", GU_ARGS(1), gu_post_quit_message },
    { "user32.dll", "ShowWindow", GU_ARGS(2), gu_show_window },
    { "user32.dll", "UpdateWindow", GU_ARGS(1), gu_update_window },
    { "user32.dll", "GetClientRect", GU_ARGS(2), gu_get_client_rect },
    { "user32.dll", "GetWindowRect", GU_ARGS(2), gu_get_window_rect },
    { "user32.dll", "AdjustWindowRect", GU_ARGS(3), gu_adjust_window_rect },
    { "user32.dll", "AdjustWindowRectEx", GU_ARGS(4), gu_adjust_window_rect_ex },
    { "user32.dll", "SetWindowLongA", GU_ARGS(3), gu_set_window_long_a },
    { "user32.dll", "SetWindowLongW", GU_ARGS(3), gu_set_window_long_w },
    { "user32.dll", "GetWindowLongA", GU_ARGS(2), gu_get_window_long_a },
    { "user32.dll", "GetWindowLongW", GU_ARGS(2), gu_get_window_long_w },
    { "user32.dll", "GetSystemMetrics", GU_ARGS(1), gu_get_system_metrics },
    { "user32.dll", "EnumDisplaySettingsA", GU_ARGS(3), gu_enum_display_settings_a },
    { "user32.dll", "EnumDisplaySettingsW", GU_ARGS(3), gu_enum_display_settings_w },
    { "user32.dll", "ShowCursor", GU_ARGS(1), gu_show_cursor },
    { "user32.dll", "SetCursor", GU_ARGS(1), gu_set_cursor },
    { "user32.dll", "LoadCursorA", GU_ARGS(2), gu_load_cursor_a },
    { "user32.dll", "LoadCursorW", GU_ARGS(2), gu_load_cursor_w },
    { "user32.dll", "LoadIconA", GU_ARGS(2), gu_load_icon_a },
    { "user32.dll", "LoadIconW", GU_ARGS(2), gu_load_icon_w },
    { "user32.dll", "MessageBoxA", GU_ARGS(4), gu_message_box_a },
    { "user32.dll", "MessageBoxW", GU_ARGS(4), gu_message_box_w }
};
const uint32_t gu_user32_export_count =
    (uint32_t)(sizeof(gu_user32_exports) / sizeof(gu_user32_exports[0]));
