/* CanLin Android adapter — the pure part (canlin#72).
 *
 * Contract: docs/superpowers/specs/2026-09-30-canlin-android-adapter-design.md §4 (lifecycle),
 * §5 (present), §6.1 (input). Everything here is plain C99 without Android headers, so the
 * host tests it (spec §9, gate "host make test"). The device layer (src/android/activity.c)
 * decodes glue commands, AInputEvents and ANativeWindow buffers into these values and
 * performs the actions; it adds nothing but the Android calls.
 *
 * Numeric values of commands, actions, formats and key codes are Android's own (glue
 * APP_CMD_*, AMOTION_EVENT_ACTION_*, WINDOW_FORMAT_*, AKEYCODE_*), so the decoding layer
 * passes them through; the device layer maps the glue enum explicitly anyway.
 */
#ifndef CANLIN_ANDROID_H
#define CANLIN_ANDROID_H

#include <stddef.h>
#include <stdint.h>

#include "canlin/android_limits.h"
#include "canlin/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle (§4) ---- */

/* Glue commands (android_native_app_glue.h APP_CMD_*, same values). */
enum {
    CANLIN_ANDROID_CMD_INPUT_CHANGED = 0,
    CANLIN_ANDROID_CMD_INIT_WINDOW = 1,
    CANLIN_ANDROID_CMD_TERM_WINDOW = 2,
    CANLIN_ANDROID_CMD_WINDOW_RESIZED = 3,
    CANLIN_ANDROID_CMD_WINDOW_REDRAW_NEEDED = 4,
    CANLIN_ANDROID_CMD_CONTENT_RECT_CHANGED = 5,
    CANLIN_ANDROID_CMD_GAINED_FOCUS = 6,
    CANLIN_ANDROID_CMD_LOST_FOCUS = 7,
    CANLIN_ANDROID_CMD_CONFIG_CHANGED = 8,
    CANLIN_ANDROID_CMD_LOW_MEMORY = 9,
    CANLIN_ANDROID_CMD_START = 10,
    CANLIN_ANDROID_CMD_RESUME = 11,
    CANLIN_ANDROID_CMD_SAVE_STATE = 12,
    CANLIN_ANDROID_CMD_PAUSE = 13,
    CANLIN_ANDROID_CMD_STOP = 14,
    CANLIN_ANDROID_CMD_DESTROY = 15
};

/* What the device layer does for a command (flags, in this order). */
enum {
    CANLIN_ANDROID_DO_WINDOW = 1u << 0,    /* take the new window: set RGBX geometry, read
                                            * size and density, resize the output */
    CANLIN_ANDROID_DO_NO_WINDOW = 1u << 1, /* forget the window before the command returns */
    CANLIN_ANDROID_DO_METRICS = 1u << 2,   /* re-read size and density; relayout */
    CANLIN_ANDROID_DO_RELEASE = 1u << 3,   /* release a held touch (as ACTION_CANCEL) */
    CANLIN_ANDROID_DO_SUSPEND = 1u << 4,   /* event runtime §7 suspension (v1: nothing
                                            * captures yet; presenting stops by state) */
    CANLIN_ANDROID_DO_RESUME = 1u << 5,    /* recheck capabilities, reconnect (v1: none) */
    CANLIN_ANDROID_DO_IMMERSIVE = 1u << 6, /* re-apply immersive mode (§7): done on the UI
                                            * thread by the activity's focus/resume
                                            * callbacks before the command arrives; nothing
                                            * left for the owner thread */
    CANLIN_ANDROID_DO_TRIM = 1u << 7,      /* low memory: log, release caches */
    CANLIN_ANDROID_DO_PRESENT = 1u << 8,   /* present the whole frame once presentable */
    CANLIN_ANDROID_DO_QUIT = 1u << 9       /* end canlin_loop_run */
};

typedef struct canlin_android_life {
    int window;   /* a native window is attached */
    int started;  /* between START and STOP */
    int resumed;  /* between RESUME and PAUSE */
    int focused;
    int quit;     /* DESTROY seen */
} canlin_android_life;

void canlin_android_life_init(canlin_android_life *l);
/* Applies one command and returns its CANLIN_ANDROID_DO_* flags. Unknown commands and
 * INPUT_CHANGED, CONTENT_RECT_CHANGED, SAVE_STATE (correctness never depends on a final
 * flush) return 0. Commands for a state already reached change nothing but still return
 * their actions. After DESTROY every command returns 0. */
unsigned canlin_android_life_cmd(canlin_android_life *l, int cmd);
/* 1 if a frame may be presented now: a window, resumed, not quitting. */
int canlin_android_life_presentable(const canlin_android_life *l);

/* ---- immersive sticky mode (§7) ---- */

/* View.SYSTEM_UI_FLAG_* (API 16/19, same values). */
enum {
    CANLIN_ANDROID_UI_HIDE_NAVIGATION = 0x0002,
    CANLIN_ANDROID_UI_FULLSCREEN = 0x0004,
    CANLIN_ANDROID_UI_LAYOUT_STABLE = 0x0100,
    CANLIN_ANDROID_UI_LAYOUT_HIDE_NAVIGATION = 0x0200,
    CANLIN_ANDROID_UI_LAYOUT_FULLSCREEN = 0x0400,
    CANLIN_ANDROID_UI_IMMERSIVE_STICKY = 0x1000
};
/* The set applied: status and navigation bars hidden, an edge swipe shows them only
 * transiently over the content, and the window keeps the full screen meanwhile. */
#define CANLIN_ANDROID_UI_IMMERSIVE                                                   \
    (CANLIN_ANDROID_UI_HIDE_NAVIGATION | CANLIN_ANDROID_UI_FULLSCREEN |                \
     CANLIN_ANDROID_UI_LAYOUT_STABLE | CANLIN_ANDROID_UI_LAYOUT_HIDE_NAVIGATION |      \
     CANLIN_ANDROID_UI_LAYOUT_FULLSCREEN | CANLIN_ANDROID_UI_IMMERSIVE_STICKY)

/* ---- input (§6.1) ---- */

/* AMOTION_EVENT_ACTION_* (masked). */
enum {
    CANLIN_ANDROID_ACTION_DOWN = 0,
    CANLIN_ANDROID_ACTION_UP = 1,
    CANLIN_ANDROID_ACTION_MOVE = 2,
    CANLIN_ANDROID_ACTION_CANCEL = 3,
    CANLIN_ANDROID_ACTION_OUTSIDE = 4,
    CANLIN_ANDROID_ACTION_POINTER_DOWN = 5,
    CANLIN_ANDROID_ACTION_POINTER_UP = 6
};

/* One decoded AMotionEvent: masked action, pointer index of the action (DOWN, UP,
 * POINTER_*), and the current position of up to CANLIN_ANDROID_POINTERS_MAX pointers in
 * window px. Historical samples are not used (motion is latest-state). */
typedef struct canlin_android_motion {
    int action;
    size_t index;
    size_t n;
    int32_t id[CANLIN_ANDROID_POINTERS_MAX];
    float x[CANLIN_ANDROID_POINTERS_MAX];
    float y[CANLIN_ANDROID_POINTERS_MAX];
} canlin_android_motion;

/* Touch state: the tracked first pointer. w, h: the output size in px (positions are
 * rounded and clamped into it); set by the device layer, 0 = no clamping. */
typedef struct canlin_android_touch {
    int active;
    int32_t id;
    int x, y;    /* last position of the tracked pointer */
    int w, h;
} canlin_android_touch;

void canlin_android_touch_init(canlin_android_touch *t, int w, int h);
/* Maps one motion event to at most one normalised input (*out): the pointer of ACTION_DOWN
 * becomes the primary button (press at DOWN, motion while it moves, release when it goes
 * up, also by POINTER_UP while others stay down). Further pointers are ignored; a new
 * tracked touch starts only with the next ACTION_DOWN. ACTION_CANCEL releases a held
 * button at its last position. Returns 1 if *out was written, else 0. */
int canlin_android_touch_map(canlin_android_touch *t, const canlin_android_motion *m,
                             canlin_ui_input *out);
/* Release of a held touch without a new position (ACTION_CANCEL, pause, window loss):
 * 1 and *out if a touch was held, else 0. */
int canlin_android_touch_release(canlin_android_touch *t, canlin_ui_input *out);

/* AKEYCODE_* used by the mapping, and AKEY_EVENT_ACTION_*. */
enum {
    CANLIN_ANDROID_KEY_BACK = 4,
    CANLIN_ANDROID_KEY_DPAD_UP = 19,
    CANLIN_ANDROID_KEY_DPAD_DOWN = 20,
    CANLIN_ANDROID_KEY_DPAD_LEFT = 21,
    CANLIN_ANDROID_KEY_DPAD_RIGHT = 22,
    CANLIN_ANDROID_KEY_VOLUME_UP = 24,
    CANLIN_ANDROID_KEY_VOLUME_DOWN = 25,
    CANLIN_ANDROID_KEY_SHIFT_LEFT = 59,
    CANLIN_ANDROID_KEY_SHIFT_RIGHT = 60,
    CANLIN_ANDROID_KEY_TAB = 61,
    CANLIN_ANDROID_KEY_ENTER = 66,
    CANLIN_ANDROID_KEY_DEL = 67,
    CANLIN_ANDROID_KEY_MENU = 82,
    CANLIN_ANDROID_KEY_ESCAPE = 111,
    CANLIN_ANDROID_KEY_FORWARD_DEL = 112,
    CANLIN_ANDROID_KEY_CTRL_LEFT = 113,
    CANLIN_ANDROID_KEY_CTRL_RIGHT = 114,
    CANLIN_ANDROID_KEY_MOVE_HOME = 122,
    CANLIN_ANDROID_KEY_MOVE_END = 123,
    CANLIN_ANDROID_KEY_NUMPAD_ENTER = 160,
    CANLIN_ANDROID_KEY_VOLUME_MUTE = 164
};
enum {
    CANLIN_ANDROID_KEY_ACTION_DOWN = 0,
    CANLIN_ANDROID_KEY_ACTION_UP = 1,
    CANLIN_ANDROID_KEY_ACTION_MULTIPLE = 2
};

/* Maps one key event. Returns 1 with *out (a KEY edge, key a canlin_ui_key or a shell key)
 * — the device layer routes it and reports the event handled; 0: handled without an
 * input (ACTION_MULTIPLE of a mapped key); -1: not handled, Android keeps it (volume keys,
 * unmapped keys). Back is Escape and always handled (the activity never finishes on Back);
 * Menu is the shell's switcher key. Text from hardware keyboards is not mapped yet. */
int canlin_android_key_map(int keycode, int action, canlin_ui_input *out);

/* DPI factor from AConfiguration_getDensity (§5): density / 160. 0 (default), 0xfffe
 * (any), 0xffff (none) and negative values count as 160; the result is clamped to
 * CANLIN_UI_DPI_MIN..CANLIN_UI_DPI_MAX. */
float canlin_android_dpi(int32_t density);

/* ---- present (§5) ---- */

/* ANativeWindow formats (WINDOW_FORMAT_*). */
enum {
    CANLIN_ANDROID_FMT_RGBA_8888 = 1,
    CANLIN_ANDROID_FMT_RGBX_8888 = 2,
    CANLIN_ANDROID_FMT_RGB_565 = 4
};

/* Row span of the dirty flags: 1 and [*y0, *y1) covering every flagged row of h, else 0. */
int canlin_android_dirty_span(const unsigned char *dirty, int h, int *y0, int *y1);

/* A locked window buffer (ANativeWindow_Buffer): stride in px. */
typedef struct canlin_android_buffer {
    void *bits;
    int32_t w, h, stride, format;
} canlin_android_buffer;

/* Writes every row of [y0, y1) (clamped to the buffer) of b from the RGB frame src (sw x sh
 * px, pitch bytes per row), converted to b's format: RGBA/RGBX as R, G, B, 255;
 * RGB_565 packed. Pixels the frame does not cover (a window larger than the frame during
 * a resize) are written black. Returns 0, or -1 for an unsupported format or a bad
 * argument (nothing written). */
int canlin_android_blit(const canlin_android_buffer *b, int y0, int y1,
                        const unsigned char *src, size_t pitch, int sw, int sh);

#ifdef __cplusplus
}
#endif

#endif
