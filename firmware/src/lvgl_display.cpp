#include "lvgl_display.h"

#include <Arduino.h>
#include <lvgl.h>

#include "lcd_panel.h"

namespace {

// Partial-render strategy (unchanged from LVGL 8):
//
// The full 800x480 RGB565 framebuffer lives in PSRAM (Arduino_GFX manages it).
// LVGL renders into a small partial buffer in internal SRAM and we hand each
// dirty rectangle off to lcd_panel::flush_area(), which writes it into the
// PSRAM framebuffer (the LCD_CAM peripheral continuously DMAs the framebuffer
// out to the panel).
//
// Buffer height = 60 lines. 800 * 60 * 2 = 96,000 bytes in internal SRAM.
constexpr uint16_t DRAW_BUF_LINES = 60;
constexpr size_t   DRAW_BUF_PX    = 800 * DRAW_BUF_LINES;

static lv_display_t* disp;
static lv_color_t    draw_buf[DRAW_BUF_PX];   // ~96 KB internal RAM (lv_color_t = uint16_t at depth 16)

// LVGL 9 flush callback: px_map is raw bytes of the rendered region.
static void flush_cb(lv_display_t* d, const lv_area_t* area, uint8_t* px_map) {
    // lv_color_t is uint16_t (RGB565) when LV_COLOR_DEPTH=16.
    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(px_map);
    lcd_panel::flush_area(area->x1, area->y1, area->x2, area->y2, pixels);
    lv_display_flush_ready(d);
}

} // namespace

namespace lvgl_display {

void init() {
    disp = lv_display_create(lcd_panel::width(), lcd_panel::height());
    lv_display_set_buffers(disp, draw_buf, nullptr,
                           sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    Serial.printf("[lvgl] display registered %ux%u draw_buf=%uKB\n",
                  lcd_panel::width(), lcd_panel::height(),
                  (unsigned)(sizeof(draw_buf) / 1024));
}

} // namespace lvgl_display
