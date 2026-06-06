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
    } else {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state   = LV_INDEV_STATE_RELEASED;
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

} // namespace lvgl_input
