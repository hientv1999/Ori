#include "lvgl_display.h"

#include <Arduino.h>
#include "ori_log.h"
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "lcd_panel.h"

namespace {

// Partial-render strategy with PSRAM draw buffer.
//
// LVGL renders dirty rectangles into draw_buf (PSRAM), then flush_cb copies
// each finished rect into the Arduino_GFX framebuffer (also PSRAM).
// LCD_CAM DMA-scans the framebuffer continuously. The intermediate copy means
// the live framebuffer is updated only with fully-rendered rectangles.
//
// Buffer height = 60 lines. 800 * 60 * 2 = 96,000 bytes in PSRAM.
constexpr uint16_t DRAW_BUF_LINES = 60;
constexpr size_t   DRAW_BUF_BYTES = 800 * DRAW_BUF_LINES * sizeof(lv_color_t);

static lv_display_t* disp;
static lv_color_t*   draw_buf;

static void flush_cb(lv_display_t* d, const lv_area_t* area, uint8_t* px_map) {
    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(px_map);
    lcd_panel::flush_area(area->x1, area->y1, area->x2, area->y2, pixels);
    lv_display_flush_ready(d);
}

} // namespace

namespace lvgl_display {

void init() {
    draw_buf = static_cast<lv_color_t*>(
        heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_SPIRAM));

    disp = lv_display_create(lcd_panel::width(), lcd_panel::height());
    lv_display_set_buffers(disp, draw_buf, nullptr,
                           DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    LOG("[lvgl] display registered %ux%u draw_buf=%uKB (psram)\n",
                  lcd_panel::width(), lcd_panel::height(),
                  (unsigned)(DRAW_BUF_BYTES / 1024));
}

} // namespace lvgl_display
