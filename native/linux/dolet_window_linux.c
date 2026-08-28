#define _GNU_SOURCE
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

enum {
    DOLET_BACKEND_NONE = 0,
    DOLET_BACKEND_WAYLAND = 1,
    DOLET_BACKEND_X11 = 2,
};

enum {
    DOLET_EVENT_NONE = 0,
    DOLET_EVENT_CLOSE = 1,
    DOLET_EVENT_RESIZE = 2,
    DOLET_EVENT_FOCUS = 3,
};

enum {
    DOLET_MODE_WINDOWED = 0,
    DOLET_MODE_FULLSCREEN = 1,
    DOLET_MODE_BORDERLESS_FULLSCREEN = 2,
};

typedef struct {
    int kind;
    int value1;
    int value2;
} DoletEvent;

typedef struct {
    struct wl_buffer *buffer;
    void *pixels;
    size_t size;
    int width;
    int height;
    int pitch;
    int busy;
} DoletWaylandBuffer;

typedef struct {
    int backend;
    int width;
    int height;
    int should_close;
    int resized;
    int focused;
    int mode;
    int cursor_visible;
    int cursor_locked;
    int mouse_x;
    int mouse_y;
    int mouse_have_last;
    int mouse_dx;
    int mouse_dy;
    float wheel;
    unsigned char keys[256];
    unsigned char mouse_buttons[8];
    char typed[512];
    int typed_len;
    int backspace;
    DoletEvent events[64];
    unsigned int event_read;
    unsigned int event_write;

    Display *x_display;
    Window x_window;
    Atom x_wm_delete;
    XImage *x_image;
    void *x_image_pixels;
    int x_image_width;
    int x_image_height;
    int x_image_pitch;
    Cursor x_blank_cursor;

    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct wl_compositor *wl_compositor;
    struct wl_shm *wl_shm;
    struct wl_seat *wl_seat;
    struct wl_surface *wl_surface;
    struct xdg_wm_base *xdg_wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_pointer *wl_pointer;
    struct wl_keyboard *wl_keyboard;
    struct zwp_relative_pointer_manager_v1 *relative_manager;
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_relative_pointer_v1 *relative_pointer;
    struct zwp_locked_pointer_v1 *locked_pointer;
    struct wl_cursor_theme *cursor_theme;
    struct wl_cursor *default_cursor;
    struct wl_surface *cursor_surface;
    uint32_t pointer_serial;
    int wayland_configured;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    DoletWaylandBuffer wl_buffers[3];
    unsigned int wl_buffer_index;
} DoletWindowState;

static DoletWindowState g;

static void push_event(int kind, int value1, int value2) {
    unsigned int next = (g.event_write + 1u) % 64u;
    if (next == g.event_read) {
        g.event_read = (g.event_read + 1u) % 64u;
    }
    g.events[g.event_write].kind = kind;
    g.events[g.event_write].value1 = value1;
    g.events[g.event_write].value2 = value2;
    g.event_write = next;
}

static void append_typed(const char *text) {
    if (!text) return;
    int n = (int)strlen(text);
    if (n <= 0) return;
    if (n > (int)sizeof(g.typed) - 1 - g.typed_len) {
        n = (int)sizeof(g.typed) - 1 - g.typed_len;
    }
    if (n <= 0) return;
    memcpy(g.typed + g.typed_len, text, (size_t)n);
    g.typed_len += n;
    g.typed[g.typed_len] = 0;
}

static int keysym_to_virtual(xkb_keysym_t sym) {
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) return 65 + (int)(sym - XKB_KEY_a);
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) return 65 + (int)(sym - XKB_KEY_A);
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) return 48 + (int)(sym - XKB_KEY_0);
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) return 112 + (int)(sym - XKB_KEY_F1);
    switch (sym) {
        case XKB_KEY_Left: return 37;
        case XKB_KEY_Up: return 38;
        case XKB_KEY_Right: return 39;
        case XKB_KEY_Down: return 40;
        case XKB_KEY_Escape: return 27;
        case XKB_KEY_space: return 32;
        case XKB_KEY_Return: return 13;
        case XKB_KEY_Tab: return 9;
        case XKB_KEY_BackSpace: return 8;
        case XKB_KEY_Delete: return 46;
        case XKB_KEY_Insert: return 45;
        case XKB_KEY_Home: return 36;
        case XKB_KEY_End: return 35;
        case XKB_KEY_Page_Up: return 33;
        case XKB_KEY_Page_Down: return 34;
        case XKB_KEY_Shift_L: return 160;
        case XKB_KEY_Shift_R: return 161;
        case XKB_KEY_Control_L: return 162;
        case XKB_KEY_Control_R: return 163;
        case XKB_KEY_Alt_L: return 164;
        case XKB_KEY_Alt_R: return 165;
        case XKB_KEY_Super_L: return 91;
        case XKB_KEY_Super_R: return 92;
        case XKB_KEY_comma: return 188;
        case XKB_KEY_period: return 190;
        case XKB_KEY_slash: return 191;
        case XKB_KEY_semicolon: return 186;
        case XKB_KEY_apostrophe: return 222;
        case XKB_KEY_bracketleft: return 219;
        case XKB_KEY_bracketright: return 221;
        case XKB_KEY_backslash: return 220;
        case XKB_KEY_minus: return 189;
        case XKB_KEY_equal: return 187;
        case XKB_KEY_grave: return 192;
        default: return 0;
    }
}

static void set_virtual_key(int virtual_key, int down) {
    if (virtual_key <= 0 || virtual_key >= 256) return;
    g.keys[virtual_key] = down ? 1u : 0u;
    if (virtual_key == 160 || virtual_key == 161) {
        g.keys[16] = (g.keys[160] || g.keys[161]) ? 1u : 0u;
    } else if (virtual_key == 162 || virtual_key == 163) {
        g.keys[17] = (g.keys[162] || g.keys[163]) ? 1u : 0u;
    } else if (virtual_key == 164 || virtual_key == 165) {
        g.keys[18] = (g.keys[164] || g.keys[165]) ? 1u : 0u;
    }
}

static void wl_buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    ((DoletWaylandBuffer *)data)->busy = 0;
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static int create_memfd(size_t size) {
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "dolet-window", 1u);
    if (fd < 0) return -1;
#else
    char name[] = "/dolet-window-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) return -1;
    unlink(name);
#endif
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void destroy_wayland_buffer(DoletWaylandBuffer *slot) {
    if (slot->buffer) wl_buffer_destroy(slot->buffer);
    if (slot->pixels && slot->size) munmap(slot->pixels, slot->size);
    memset(slot, 0, sizeof(*slot));
}

static int ensure_wayland_buffer(DoletWaylandBuffer *slot, int width, int height, int pitch) {
    size_t size = (size_t)pitch * (size_t)height;
    if (slot->buffer && slot->width == width && slot->height == height && slot->pitch == pitch) {
        return 1;
    }
    if (slot->busy && g.wl_display) wl_display_roundtrip(g.wl_display);
    destroy_wayland_buffer(slot);
    int fd = create_memfd(size);
    if (fd < 0) return 0;
    void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return 0;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(g.wl_shm, fd, (int)size);
    if (!pool) {
        munmap(pixels, size);
        close(fd);
        return 0;
    }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, pitch, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!buffer) {
        munmap(pixels, size);
        return 0;
    }
    slot->buffer = buffer;
    slot->pixels = pixels;
    slot->size = size;
    slot->width = width;
    slot->height = height;
    slot->pitch = pitch;
    slot->busy = 0;
    wl_buffer_add_listener(buffer, &wl_buffer_listener, slot);
    return 1;
}

static void xdg_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(surface, serial);
    g.wayland_configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height, struct wl_array *states) {
    (void)data; (void)toplevel; (void)states;
    if (width > 0 && height > 0 && (width != g.width || height != g.height)) {
        g.width = width;
        g.height = height;
        g.resized = 1;
        push_event(DOLET_EVENT_RESIZE, width, height);
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data; (void)toplevel;
    g.should_close = 1;
    push_event(DOLET_EVENT_CLOSE, 0, 0);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)data; (void)surface;
    g.pointer_serial = serial;
    g.mouse_x = wl_fixed_to_int(sx);
    g.mouse_y = wl_fixed_to_int(sy);
    g.mouse_have_last = 1;
    if (!g.cursor_visible) {
        wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
    } else if (g.default_cursor && g.cursor_surface && g.default_cursor->image_count > 0) {
        struct wl_cursor_image *image = g.default_cursor->images[0];
        struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
        wl_pointer_set_cursor(pointer, serial, g.cursor_surface,
                              (int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
        wl_surface_attach(g.cursor_surface, buffer, 0, 0);
        wl_surface_damage(g.cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
        wl_surface_commit(g.cursor_surface);
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) {
    (void)data; (void)pointer; (void)serial; (void)surface;
    g.mouse_have_last = 0;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy) {
    (void)data; (void)pointer; (void)time;
    int x = wl_fixed_to_int(sx);
    int y = wl_fixed_to_int(sy);
    if (g.cursor_locked && !g.relative_pointer && g.mouse_have_last) {
        g.mouse_dx += x - g.mouse_x;
        g.mouse_dy += y - g.mouse_y;
    }
    g.mouse_x = x;
    g.mouse_y = y;
    g.mouse_have_last = 1;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    (void)data; (void)pointer; (void)serial; (void)time;
    int down = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (button == BTN_LEFT) {
        g.mouse_buttons[1] = down;
        g.keys[1] = down;
    } else if (button == BTN_RIGHT) {
        g.mouse_buttons[2] = down;
        g.keys[2] = down;
    } else if (button == BTN_MIDDLE) {
        g.mouse_buttons[4] = down;
        g.keys[4] = down;
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    (void)data; (void)pointer; (void)time;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        double amount = wl_fixed_to_double(value);
        if (amount < 0.0) g.wheel += 1.0f;
        else if (amount > 0.0) g.wheel -= 1.0f;
    }
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t source) {
    (void)data; (void)pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis) {
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete) {
    /* The paired continuous axis event already updates the public wheel
       accumulator; consuming both would double every wheel step. */
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void relative_motion(void *data, struct zwp_relative_pointer_v1 *pointer,
                            uint32_t time_hi, uint32_t time_lo,
                            wl_fixed_t dx, wl_fixed_t dy,
                            wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
    (void)data; (void)pointer; (void)time_hi; (void)time_lo; (void)dx; (void)dy;
    if (!g.cursor_locked) return;
    g.mouse_dx += (int)wl_fixed_to_double(dx_unaccel);
    g.mouse_dy += (int)wl_fixed_to_double(dy_unaccel);
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = relative_motion,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                            int fd, uint32_t size) {
    (void)data; (void)keyboard;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    if (!g.xkb_context) g.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        g.xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (!keymap) return;
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        return;
    }
    if (g.xkb_state) xkb_state_unref(g.xkb_state);
    if (g.xkb_keymap) xkb_keymap_unref(g.xkb_keymap);
    g.xkb_keymap = keymap;
    g.xkb_state = state;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
    if (!g.focused) {
        g.focused = 1;
        push_event(DOLET_EVENT_FOCUS, 1, 0);
    }
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface) {
    (void)data; (void)keyboard; (void)serial; (void)surface;
    memset(g.keys, 0, sizeof(g.keys));
    if (g.focused) {
        g.focused = 0;
        push_event(DOLET_EVENT_FOCUS, 0, 0);
    }
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state) {
    (void)data; (void)keyboard; (void)serial; (void)time;
    if (!g.xkb_state) return;
    xkb_keycode_t code = key + 8u;
    int down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(g.xkb_state, code);
    int virtual_key = keysym_to_virtual(sym);
    set_virtual_key(virtual_key, down);
    if (down) {
        if (sym == XKB_KEY_BackSpace) {
            g.backspace = 1;
        } else {
            char text[64];
            int length = xkb_state_key_get_utf8(g.xkb_state, code, text, sizeof(text));
            if (length > 0) append_typed(text);
        }
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched,
                               uint32_t locked, uint32_t group) {
    (void)data; (void)keyboard; (void)serial;
    if (g.xkb_state) xkb_state_update_mask(g.xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
    (void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.wl_pointer) {
        g.wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g.wl_pointer, &pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && g.wl_pointer) {
        wl_pointer_destroy(g.wl_pointer);
        g.wl_pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g.wl_keyboard) {
        g.wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g.wl_keyboard, &keyboard_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && g.wl_keyboard) {
        wl_keyboard_destroy(g.wl_keyboard);
        g.wl_keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t v = version < 4 ? version : 4;
        g.wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, v);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        g.wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t v = version < 5 ? version : 5;
        g.wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, v);
        wl_seat_add_listener(g.wl_seat, &seat_listener, NULL);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        g.xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(g.xdg_wm_base, &xdg_wm_base_listener, NULL);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        g.relative_manager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        g.pointer_constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static int open_wayland(const char *title, int width, int height) {
    g.wl_display = wl_display_connect(NULL);
    if (!g.wl_display) return 0;
    g.wl_registry = wl_display_get_registry(g.wl_display);
    if (!g.wl_registry) return 0;
    wl_registry_add_listener(g.wl_registry, &registry_listener, NULL);
    if (wl_display_roundtrip(g.wl_display) < 0) return 0;
    if (wl_display_roundtrip(g.wl_display) < 0) return 0;
    if (!g.wl_compositor || !g.wl_shm || !g.xdg_wm_base) return 0;

    g.wl_surface = wl_compositor_create_surface(g.wl_compositor);
    if (!g.wl_surface) return 0;
    g.xdg_surface = xdg_wm_base_get_xdg_surface(g.xdg_wm_base, g.wl_surface);
    if (!g.xdg_surface) return 0;
    xdg_surface_add_listener(g.xdg_surface, &xdg_surface_listener, NULL);
    g.xdg_toplevel = xdg_surface_get_toplevel(g.xdg_surface);
    if (!g.xdg_toplevel) return 0;
    xdg_toplevel_add_listener(g.xdg_toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(g.xdg_toplevel, title ? title : "Dolet");
    xdg_toplevel_set_app_id(g.xdg_toplevel, "dolet.application");
    wl_surface_commit(g.wl_surface);
    while (!g.wayland_configured) {
        if (wl_display_dispatch(g.wl_display) < 0) return 0;
    }

    g.cursor_surface = wl_compositor_create_surface(g.wl_compositor);
    g.cursor_theme = wl_cursor_theme_load(NULL, 24, g.wl_shm);
    if (g.cursor_theme) g.default_cursor = wl_cursor_theme_get_cursor(g.cursor_theme, "left_ptr");
    g.backend = DOLET_BACKEND_WAYLAND;
    g.width = width;
    g.height = height;
    return 1;
}

static int open_x11(const char *title, int width, int height) {
    g.x_display = XOpenDisplay(NULL);
    if (!g.x_display) return 0;
    int screen = DefaultScreen(g.x_display);
    Window root = RootWindow(g.x_display, screen);
    g.x_window = XCreateSimpleWindow(g.x_display, root, 64, 64,
                                     (unsigned int)width, (unsigned int)height, 0,
                                     BlackPixel(g.x_display, screen), BlackPixel(g.x_display, screen));
    if (!g.x_window) return 0;
    long mask = StructureNotifyMask | ExposureMask | PointerMotionMask |
                ButtonPressMask | ButtonReleaseMask | FocusChangeMask |
                KeyPressMask | KeyReleaseMask;
    XSelectInput(g.x_display, g.x_window, mask);
    XStoreName(g.x_display, g.x_window, title ? title : "Dolet");
    g.x_wm_delete = XInternAtom(g.x_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g.x_display, g.x_window, &g.x_wm_delete, 1);
    XMapWindow(g.x_display, g.x_window);
    XFlush(g.x_display);
    g.backend = DOLET_BACKEND_X11;
    g.width = width;
    g.height = height;
    return 1;
}

static void destroy_ximage(void) {
    if (!g.x_image) return;
    g.x_image->data = NULL;
    XDestroyImage(g.x_image);
    g.x_image = NULL;
    g.x_image_pixels = NULL;
    g.x_image_width = 0;
    g.x_image_height = 0;
    g.x_image_pitch = 0;
}

static void close_wayland(void) {
    for (int i = 0; i < 3; ++i) destroy_wayland_buffer(&g.wl_buffers[i]);
    if (g.locked_pointer) zwp_locked_pointer_v1_destroy(g.locked_pointer);
    if (g.relative_pointer) zwp_relative_pointer_v1_destroy(g.relative_pointer);
    if (g.wl_keyboard) wl_keyboard_destroy(g.wl_keyboard);
    if (g.wl_pointer) wl_pointer_destroy(g.wl_pointer);
    if (g.cursor_theme) wl_cursor_theme_destroy(g.cursor_theme);
    if (g.cursor_surface) wl_surface_destroy(g.cursor_surface);
    if (g.xdg_toplevel) xdg_toplevel_destroy(g.xdg_toplevel);
    if (g.xdg_surface) xdg_surface_destroy(g.xdg_surface);
    if (g.wl_surface) wl_surface_destroy(g.wl_surface);
    if (g.pointer_constraints) zwp_pointer_constraints_v1_destroy(g.pointer_constraints);
    if (g.relative_manager) zwp_relative_pointer_manager_v1_destroy(g.relative_manager);
    if (g.xdg_wm_base) xdg_wm_base_destroy(g.xdg_wm_base);
    if (g.wl_seat) wl_seat_destroy(g.wl_seat);
    if (g.wl_shm) wl_shm_destroy(g.wl_shm);
    if (g.wl_compositor) wl_compositor_destroy(g.wl_compositor);
    if (g.wl_registry) wl_registry_destroy(g.wl_registry);
    if (g.xkb_state) xkb_state_unref(g.xkb_state);
    if (g.xkb_keymap) xkb_keymap_unref(g.xkb_keymap);
    if (g.xkb_context) xkb_context_unref(g.xkb_context);
    if (g.wl_display) wl_display_disconnect(g.wl_display);
}

static void close_x11(void) {
    destroy_ximage();
    if (g.x_blank_cursor && g.x_display) XFreeCursor(g.x_display, g.x_blank_cursor);
    if (g.x_window && g.x_display) XDestroyWindow(g.x_display, g.x_window);
    if (g.x_display) {
        XFlush(g.x_display);
        XCloseDisplay(g.x_display);
    }
}

int dolet_window_open(const char *title, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    memset(&g, 0, sizeof(g));
    g.focused = 1;
    g.cursor_visible = 1;
    g.mode = DOLET_MODE_WINDOWED;
    const char *requested = getenv("DOLET_WINDOW_BACKEND");
    int force_x11 = requested && strcmp(requested, "x11") == 0;
    int force_wayland = requested && strcmp(requested, "wayland") == 0;
    if (!force_x11 && open_wayland(title, width, height)) return 1;
    if (g.wl_display) {
        close_wayland();
        memset(&g, 0, sizeof(g));
        g.focused = 1;
        g.cursor_visible = 1;
    }
    if (!force_wayland && open_x11(title, width, height)) return 1;
    if (g.x_display) close_x11();
    memset(&g, 0, sizeof(g));
    return 0;
}

void dolet_window_close(void) {
    int backend = g.backend;
    if (backend == DOLET_BACKEND_WAYLAND) close_wayland();
    else if (backend == DOLET_BACKEND_X11) close_x11();
    memset(&g, 0, sizeof(g));
}

static int x11_virtual_key(KeySym sym) {
    return keysym_to_virtual((xkb_keysym_t)sym);
}

static void poll_x11(void) {
    while (g.x_display && XPending(g.x_display) > 0) {
        XEvent event;
        XNextEvent(g.x_display, &event);
        switch (event.type) {
            case ClientMessage:
                if ((Atom)event.xclient.data.l[0] == g.x_wm_delete) {
                    g.should_close = 1;
                    push_event(DOLET_EVENT_CLOSE, 0, 0);
                }
                break;
            case DestroyNotify:
                g.should_close = 1;
                push_event(DOLET_EVENT_CLOSE, 0, 0);
                break;
            case ConfigureNotify:
                if (event.xconfigure.width > 0 && event.xconfigure.height > 0 &&
                    (event.xconfigure.width != g.width || event.xconfigure.height != g.height)) {
                    g.width = event.xconfigure.width;
                    g.height = event.xconfigure.height;
                    g.resized = 1;
                    push_event(DOLET_EVENT_RESIZE, g.width, g.height);
                }
                break;
            case FocusIn:
                g.focused = 1;
                push_event(DOLET_EVENT_FOCUS, 1, 0);
                break;
            case FocusOut:
                g.focused = 0;
                g.mouse_have_last = 0;
                memset(g.keys, 0, sizeof(g.keys));
                push_event(DOLET_EVENT_FOCUS, 0, 0);
                break;
            case MotionNotify: {
                int x = event.xmotion.x;
                int y = event.xmotion.y;
                if (g.cursor_locked && g.mouse_have_last) {
                    g.mouse_dx += x - g.mouse_x;
                    g.mouse_dy += y - g.mouse_y;
                }
                g.mouse_x = x;
                g.mouse_y = y;
                g.mouse_have_last = 1;
                if (g.cursor_locked && g.width > 96 && g.height > 96 &&
                    (x < 48 || x > g.width - 48 || y < 48 || y > g.height - 48)) {
                    int cx = g.width / 2;
                    int cy = g.height / 2;
                    g.mouse_have_last = 0;
                    XWarpPointer(g.x_display, None, g.x_window, 0, 0, 0, 0, cx, cy);
                    XFlush(g.x_display);
                    g.mouse_x = cx;
                    g.mouse_y = cy;
                    g.mouse_have_last = 1;
                }
                break;
            }
            case ButtonPress:
            case ButtonRelease: {
                int down = event.type == ButtonPress;
                if (event.xbutton.button == Button1) g.keys[1] = g.mouse_buttons[1] = down;
                else if (event.xbutton.button == Button3) g.keys[2] = g.mouse_buttons[2] = down;
                else if (event.xbutton.button == Button2) g.keys[4] = g.mouse_buttons[4] = down;
                else if (down && event.xbutton.button == Button4) g.wheel += 1.0f;
                else if (down && event.xbutton.button == Button5) g.wheel -= 1.0f;
                break;
            }
            case KeyPress:
            case KeyRelease: {
                KeySym sym = XLookupKeysym(&event.xkey, 0);
                int virtual_key = x11_virtual_key(sym);
                int down = event.type == KeyPress;
                set_virtual_key(virtual_key, down);
                if (down) {
                    char text[64];
                    KeySym ignored;
                    int count = XLookupString(&event.xkey, text, (int)sizeof(text) - 1, &ignored, NULL);
                    if (sym == XK_BackSpace) g.backspace = 1;
                    else if (count > 0) {
                        text[count] = 0;
                        append_typed(text);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}

static int poll_wayland(int timeout_ms) {
    if (g.wl_display) {
        int dispatched = wl_display_dispatch_pending(g.wl_display);
        if (dispatched < 0) goto wayland_closed;
        if (dispatched > 0) timeout_ms = 0;
        while (wl_display_prepare_read(g.wl_display) != 0) {
            dispatched = wl_display_dispatch_pending(g.wl_display);
            if (dispatched < 0) goto wayland_closed;
            if (dispatched > 0) timeout_ms = 0;
        }
        if (wl_display_flush(g.wl_display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(g.wl_display);
            goto wayland_closed;
        }
        struct pollfd fd = { .fd = wl_display_get_fd(g.wl_display), .events = POLLIN, .revents = 0 };
        int ready = poll(&fd, 1, timeout_ms);
        if (ready > 0 && (fd.revents & POLLIN)) {
            if (wl_display_read_events(g.wl_display) < 0) goto wayland_closed;
            if (wl_display_dispatch_pending(g.wl_display) < 0) goto wayland_closed;
        } else {
            wl_display_cancel_read(g.wl_display);
            if (ready > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) goto wayland_closed;
        }
    }
    return 1;

wayland_closed:
    g.should_close = 1;
    push_event(DOLET_EVENT_CLOSE, 0, 0);
    return 0;
}

void dolet_window_poll(void) {
    if (g.backend == DOLET_BACKEND_WAYLAND && g.wl_display) {
        poll_wayland(0);
    } else if (g.backend == DOLET_BACKEND_X11) {
        poll_x11();
    }
}

void dolet_window_wait_timeout(int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = -1;
    if (g.backend == DOLET_BACKEND_WAYLAND && g.wl_display) {
        poll_wayland(timeout_ms);
    } else if (g.backend == DOLET_BACKEND_X11 && g.x_display) {
        if (XPending(g.x_display) == 0) {
            struct pollfd fd = {
                .fd = ConnectionNumber(g.x_display),
                .events = POLLIN,
                .revents = 0,
            };
            poll(&fd, 1, timeout_ms);
        }
        poll_x11();
    }
}

int dolet_window_should_close(void) { return g.backend == DOLET_BACKEND_NONE || g.should_close; }
void dolet_window_request_close(void) { g.should_close = 1; }
int dolet_window_width(void) { return g.width; }
int dolet_window_height(void) { return g.height; }
int dolet_window_focused(void) { return g.focused; }
int dolet_window_resized(void) { return g.resized; }
void dolet_window_reset_resized(void) { g.resized = 0; }
int dolet_window_backend(void) { return g.backend; }
const char *dolet_window_backend_name(void) {
    if (g.backend == DOLET_BACKEND_WAYLAND) return "wayland";
    if (g.backend == DOLET_BACKEND_X11) return "x11";
    return "none";
}
uint64_t dolet_window_handle(void) {
    if (g.backend == DOLET_BACKEND_WAYLAND) return (uint64_t)(uintptr_t)g.wl_surface;
    if (g.backend == DOLET_BACKEND_X11) return (uint64_t)g.x_window;
    return 0;
}
uint64_t dolet_window_display(void) {
    if (g.backend == DOLET_BACKEND_WAYLAND) return (uint64_t)(uintptr_t)g.wl_display;
    if (g.backend == DOLET_BACKEND_X11) return (uint64_t)(uintptr_t)g.x_display;
    return 0;
}

int dolet_window_next_event(int *kind, int *value1, int *value2) {
    if (g.event_read == g.event_write) return 0;
    DoletEvent *event = &g.events[g.event_read];
    if (kind) *kind = event->kind;
    if (value1) *value1 = event->value1;
    if (value2) *value2 = event->value2;
    g.event_read = (g.event_read + 1u) % 64u;
    return 1;
}

float dolet_window_consume_wheel(void) { float v = g.wheel; g.wheel = 0.0f; return v; }
int dolet_window_consume_mouse_dx(void) { int v = g.mouse_dx; g.mouse_dx = 0; return v; }
int dolet_window_consume_mouse_dy(void) { int v = g.mouse_dy; g.mouse_dy = 0; return v; }
const char *dolet_window_consume_typed(void) {
    static char drained[512];
    memcpy(drained, g.typed, sizeof(drained));
    g.typed_len = 0;
    g.typed[0] = 0;
    return drained;
}
int dolet_window_consume_backspace(void) { int v = g.backspace; g.backspace = 0; return v; }
int dolet_window_key_down(int key) { return key >= 0 && key < 256 ? g.keys[key] != 0 : 0; }
int dolet_window_mouse_button_down(int button) {
    return button >= 0 && button < 8 ? g.mouse_buttons[button] != 0 : 0;
}
void dolet_window_clear_mouse_delta(void) { g.mouse_dx = g.mouse_dy = 0; g.mouse_have_last = 0; }
int dolet_window_mouse_x(void) { return g.mouse_x; }
int dolet_window_mouse_y(void) { return g.mouse_y; }

static void wayland_update_cursor(void) {
    if (!g.wl_pointer || !g.pointer_serial) return;
    if (!g.cursor_visible) {
        wl_pointer_set_cursor(g.wl_pointer, g.pointer_serial, NULL, 0, 0);
        return;
    }
    if (!g.default_cursor || !g.cursor_surface || g.default_cursor->image_count == 0) return;
    struct wl_cursor_image *image = g.default_cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    wl_pointer_set_cursor(g.wl_pointer, g.pointer_serial, g.cursor_surface,
                          (int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
    wl_surface_attach(g.cursor_surface, buffer, 0, 0);
    wl_surface_damage(g.cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(g.cursor_surface);
}

void dolet_window_set_cursor_visible(int visible) {
    g.cursor_visible = visible != 0;
    if (g.backend == DOLET_BACKEND_WAYLAND) {
        wayland_update_cursor();
    } else if (g.backend == DOLET_BACKEND_X11 && g.x_display && g.x_window) {
        if (g.cursor_visible) {
            XUndefineCursor(g.x_display, g.x_window);
        } else {
            if (!g.x_blank_cursor) {
                char data[1] = {0};
                Pixmap blank = XCreateBitmapFromData(g.x_display, g.x_window, data, 1, 1);
                XColor color;
                memset(&color, 0, sizeof(color));
                g.x_blank_cursor = XCreatePixmapCursor(g.x_display, blank, blank, &color, &color, 0, 0);
                XFreePixmap(g.x_display, blank);
            }
            XDefineCursor(g.x_display, g.x_window, g.x_blank_cursor);
        }
        XFlush(g.x_display);
    }
}

int dolet_window_cursor_visible(void) { return g.cursor_visible; }

void dolet_window_set_cursor_locked(int locked) {
    locked = locked != 0;
    if (locked == g.cursor_locked) return;
    g.cursor_locked = locked;
    dolet_window_clear_mouse_delta();
    if (g.backend == DOLET_BACKEND_WAYLAND && g.wl_pointer) {
        if (locked) {
            if (g.relative_manager && !g.relative_pointer) {
                g.relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
                    g.relative_manager, g.wl_pointer);
                if (g.relative_pointer) {
                    zwp_relative_pointer_v1_add_listener(g.relative_pointer, &relative_pointer_listener, NULL);
                }
            }
            if (g.pointer_constraints && !g.locked_pointer) {
                g.locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
                    g.pointer_constraints, g.wl_surface, g.wl_pointer, NULL,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            }
        } else {
            if (g.locked_pointer) {
                zwp_locked_pointer_v1_destroy(g.locked_pointer);
                g.locked_pointer = NULL;
            }
            if (g.relative_pointer) {
                zwp_relative_pointer_v1_destroy(g.relative_pointer);
                g.relative_pointer = NULL;
            }
        }
    } else if (g.backend == DOLET_BACKEND_X11 && locked && g.x_display && g.x_window) {
        int cx = g.width / 2;
        int cy = g.height / 2;
        XWarpPointer(g.x_display, None, g.x_window, 0, 0, 0, 0, cx, cy);
        XFlush(g.x_display);
        g.mouse_x = cx;
        g.mouse_y = cy;
        g.mouse_have_last = 1;
    }
}

int dolet_window_cursor_locked(void) { return g.cursor_locked; }
void dolet_window_set_cursor_position(int x, int y) {
    if (g.backend == DOLET_BACKEND_X11 && g.x_display && g.x_window) {
        XWarpPointer(g.x_display, None, g.x_window, 0, 0, 0, 0, x, y);
        XFlush(g.x_display);
    }
    g.mouse_x = x;
    g.mouse_y = y;
}
void dolet_window_center_cursor(void) { dolet_window_set_cursor_position(g.width / 2, g.height / 2); }

void dolet_window_set_title(const char *title) {
    if (g.backend == DOLET_BACKEND_WAYLAND && g.xdg_toplevel) {
        xdg_toplevel_set_title(g.xdg_toplevel, title ? title : "Dolet");
        wl_display_flush(g.wl_display);
    } else if (g.backend == DOLET_BACKEND_X11 && g.x_display && g.x_window) {
        XStoreName(g.x_display, g.x_window, title ? title : "Dolet");
        XFlush(g.x_display);
    }
}

int dolet_window_set_mode(int mode, int width, int height) {
    if (mode < DOLET_MODE_WINDOWED || mode > DOLET_MODE_BORDERLESS_FULLSCREEN) return 0;
    g.mode = mode;
    if (g.backend == DOLET_BACKEND_WAYLAND && g.xdg_toplevel) {
        if (mode == DOLET_MODE_WINDOWED) xdg_toplevel_unset_fullscreen(g.xdg_toplevel);
        else xdg_toplevel_set_fullscreen(g.xdg_toplevel, NULL);
        wl_surface_commit(g.wl_surface);
    } else if (g.backend == DOLET_BACKEND_X11 && g.x_display && g.x_window) {
        Atom wm_state = XInternAtom(g.x_display, "_NET_WM_STATE", False);
        Atom fullscreen = XInternAtom(g.x_display, "_NET_WM_STATE_FULLSCREEN", False);
        XEvent event;
        memset(&event, 0, sizeof(event));
        event.type = ClientMessage;
        event.xclient.window = g.x_window;
        event.xclient.message_type = wm_state;
        event.xclient.format = 32;
        event.xclient.data.l[0] = mode == DOLET_MODE_WINDOWED ? 0 : 1;
        event.xclient.data.l[1] = (long)fullscreen;
        XSendEvent(g.x_display, DefaultRootWindow(g.x_display), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
        if (mode == DOLET_MODE_WINDOWED && width > 0 && height > 0) {
            XResizeWindow(g.x_display, g.x_window, (unsigned int)width, (unsigned int)height);
        }
        XFlush(g.x_display);
    }
    return 1;
}

int dolet_window_mode(void) { return g.mode; }

int dolet_window_present(const void *pixels, int width, int height, int pitch) {
    if (!pixels || width <= 0 || height <= 0 || pitch < width * 4) return 0;
    if (g.backend == DOLET_BACKEND_WAYLAND) {
        if (!g.wl_surface || !g.wl_shm) return 0;
        DoletWaylandBuffer *slot = NULL;
        for (int attempt = 0; attempt < 3; ++attempt) {
            unsigned int index = (g.wl_buffer_index + (unsigned int)attempt) % 3u;
            if (!g.wl_buffers[index].busy) {
                slot = &g.wl_buffers[index];
                g.wl_buffer_index = (index + 1u) % 3u;
                break;
            }
        }
        if (!slot) {
            if (wl_display_dispatch(g.wl_display) < 0) return 0;
            for (int i = 0; i < 3; ++i) {
                if (!g.wl_buffers[i].busy) { slot = &g.wl_buffers[i]; break; }
            }
        }
        if (!slot || !ensure_wayland_buffer(slot, width, height, pitch)) return 0;
        memcpy(slot->pixels, pixels, (size_t)pitch * (size_t)height);
        slot->busy = 1;
        wl_surface_attach(g.wl_surface, slot->buffer, 0, 0);
        wl_surface_damage(g.wl_surface, 0, 0, width, height);
        wl_surface_commit(g.wl_surface);
        return wl_display_flush(g.wl_display) >= 0 || errno == EAGAIN;
    }
    if (g.backend == DOLET_BACKEND_X11 && g.x_display && g.x_window) {
        int changed = !g.x_image || g.x_image_pixels != pixels ||
                      g.x_image_width != width || g.x_image_height != height ||
                      g.x_image_pitch != pitch;
        if (changed) {
            destroy_ximage();
            int screen = DefaultScreen(g.x_display);
            g.x_image = XCreateImage(g.x_display, DefaultVisual(g.x_display, screen),
                                     (unsigned int)DefaultDepth(g.x_display, screen), ZPixmap,
                                     0, (char *)pixels, width, height, 32, pitch);
            if (!g.x_image) return 0;
            g.x_image_pixels = (void *)pixels;
            g.x_image_width = width;
            g.x_image_height = height;
            g.x_image_pitch = pitch;
        }
        GC gc = DefaultGC(g.x_display, DefaultScreen(g.x_display));
        XPutImage(g.x_display, g.x_window, gc, g.x_image, 0, 0, 0, 0,
                  (unsigned int)width, (unsigned int)height);
        XFlush(g.x_display);
        return 1;
    }
    return 0;
}

const char *dolet_window_vulkan_extension(void) {
    if (g.backend == DOLET_BACKEND_WAYLAND) return "VK_KHR_wayland_surface";
    if (g.backend == DOLET_BACKEND_X11) return "VK_KHR_xlib_surface";
    return "";
}

uint64_t dolet_window_create_vulkan_surface(uint64_t instance_value) {
    if (!instance_value) return 0;
    void *vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan) vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan) return 0;
    VkInstance instance = (VkInstance)(uintptr_t)instance_value;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;
    if (g.backend == DOLET_BACKEND_WAYLAND) {
        PFN_vkCreateWaylandSurfaceKHR create_wayland =
            (PFN_vkCreateWaylandSurfaceKHR)dlsym(vulkan, "vkCreateWaylandSurfaceKHR");
        if (create_wayland) {
            VkWaylandSurfaceCreateInfoKHR info;
            memset(&info, 0, sizeof(info));
            info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
            info.display = g.wl_display;
            info.surface = g.wl_surface;
            result = create_wayland(instance, &info, NULL, &surface);
        }
    } else if (g.backend == DOLET_BACKEND_X11) {
        PFN_vkCreateXlibSurfaceKHR create_xlib =
            (PFN_vkCreateXlibSurfaceKHR)dlsym(vulkan, "vkCreateXlibSurfaceKHR");
        if (create_xlib) {
            VkXlibSurfaceCreateInfoKHR info;
            memset(&info, 0, sizeof(info));
            info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
            info.dpy = g.x_display;
            info.window = g.x_window;
            result = create_xlib(instance, &info, NULL, &surface);
        }
    }
    dlclose(vulkan);
    return result == VK_SUCCESS ? (uint64_t)(uintptr_t)surface : 0;
}

void dolet_window_destroy_vulkan_surface(uint64_t instance_value, uint64_t surface_value) {
    if (!instance_value || !surface_value) return;
    void *vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan) vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan) return;
    PFN_vkDestroySurfaceKHR destroy_surface =
        (PFN_vkDestroySurfaceKHR)dlsym(vulkan, "vkDestroySurfaceKHR");
    if (destroy_surface) {
        destroy_surface((VkInstance)(uintptr_t)instance_value,
                        (VkSurfaceKHR)(uintptr_t)surface_value, NULL);
    }
    dlclose(vulkan);
}

void dolet_window_set_size(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (g.backend == DOLET_BACKEND_X11 && g.x_display && g.x_window) {
        XResizeWindow(g.x_display, g.x_window, (unsigned int)width, (unsigned int)height);
        XFlush(g.x_display);
    }
    g.width = width;
    g.height = height;
}

void dolet_window_show(int visible) {
    if (g.backend == DOLET_BACKEND_X11 && g.x_display && g.x_window) {
        if (visible) XMapWindow(g.x_display, g.x_window);
        else XUnmapWindow(g.x_display, g.x_window);
        XFlush(g.x_display);
    }
    /* xdg-shell deliberately has no arbitrary map/unmap request. */
}
