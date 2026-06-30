#include "lvgl_input.h"

#include <Arduino.h>
#include "ori_log.h"
#include <lvgl.h>

#include "lcd_panel.h"

namespace {

static lv_indev_t* indev;

// Cached state populated by feed() from the single touch::poll() that
// main.cpp performs every loop tick.
static TouchPoint cached_points[5];
static uint8_t    cached_n = 0;

// Last reported coordinates — LVGL expects them to remain valid through
// the release frame so it can compute click-vs-drag deltas.
static int16_t last_x = 0;
static int16_t last_y = 0;

static int16_t clamp_i16(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return (int16_t)lo;
    if (v > hi) return (int16_t)hi;
    return (int16_t)v;
}

// ── Tap-slop guard ───────────────────────────────────────────────────────────
// If the finger travels more than TAP_SLOP_PX from where it first touched down,
// the press is cancelled (no CLICKED fires) — so an accidental touch that the
// user slides away from and releases does nothing. Implemented via LVGL's
// lv_indev_wait_release(), which drops the current press (PRESS_LOST, no click)
// until the finger lifts. Two exemptions:
//   • Scrolling — LVGL already cancels the click once a scroll engages, and
//     aborting the press would freeze the scroll. Skip when a scroll is active.
//   • Swipe surfaces (album art) — they interpret their own drag distance for
//     prev/next/volume gestures. They opt out with LV_OBJ_FLAG_USER_1.
static constexpr int32_t TAP_SLOP_PX = 40;
static bool    press_active    = false;  // a touch is currently down
static bool    press_cancelled = false;  // slop already exceeded for this touch
static int16_t press_x = 0, press_y = 0; // touch-down point
static bool    slop_suspended = false;   // set by swipe surfaces (album art)

// LVGL 9 read callback: first arg is lv_indev_t*, not lv_indev_drv_t*.
static void read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    const int16_t maxX = (int16_t)lcd_panel::width()  - 1;
    const int16_t maxY = (int16_t)lcd_panel::height() - 1;

    if (cached_n >= 1 && cached_points[0].pressed) {
        last_x = clamp_i16(cached_points[0].x, 0, maxX);
        last_y = clamp_i16(cached_points[0].y, 0, maxY);
        data->point.x = last_x;
        data->point.y = last_y;
        data->state   = LV_INDEV_STATE_PRESSED;

        if (!press_active) {                 // touch-down: remember the origin
            press_active    = true;
            press_cancelled = false;
            press_x = last_x;
            press_y = last_y;
            // Reset any stale suspend; a swipe surface re-arms it via its own
            // PRESSED handler in this same frame's event processing.
            slop_suspended = false;
        } else if (!press_cancelled) {        // dragging: check the slop once
            int32_t dx = (int32_t)last_x - press_x;
            int32_t dy = (int32_t)last_y - press_y;
            if (dx * dx + dy * dy > TAP_SLOP_PX * TAP_SLOP_PX) {
                // Don't fight a scroll (LVGL already cancels its click and
                // aborting the press would freeze the scroll) or a swipe surface.
                // NOTE: lv_indev_get_active_obj() is unusable here — LVGL nulls
                // indev_obj_act at the end of every read, before this runs — so
                // swipe surfaces opt out via suspend_tap_slop() instead.
                if (!slop_suspended && lv_indev_get_scroll_obj(indev) == nullptr) {
                    lv_indev_wait_release(indev);  // abort the tap; no CLICKED
                }
                press_cancelled = true;            // decide only once per touch
            }
        }
    } else {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state   = LV_INDEV_STATE_RELEASED;
        press_active    = false;
        press_cancelled = false;
    }
}

} // namespace

namespace lvgl_input {

void init() {
    cached_n = 0;
    for (auto& p : cached_points) { p.x = 0; p.y = 0; p.pressed = false; }

    // LVGL 9 API: create an indev object, then set its type and read callback.
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);

    LOG("[lvgl] input registered (gt911 pointer)\n");
}

lv_indev_t* get() {
    return indev;
}

void feed(const TouchPoint* points, uint8_t n) {
    if (n > 5) n = 5;
    cached_n = n;
    for (uint8_t i = 0; i < n; ++i) cached_points[i] = points[i];
}

void suspend_tap_slop(bool suspend) {
    slop_suspended = suspend;
}

} // namespace lvgl_input
