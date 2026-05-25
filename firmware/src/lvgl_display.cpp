#include "lvgl_display.h"

#include <Arduino.h>
#include <lvgl.h>

#include "lcd_panel.h"

namespace {

// Partial-render strategy:
//
// The full 800x480 RGB565 framebuffer lives in PSRAM (Arduino_GFX manages it).
// LVGL renders into a small partial buffer in internal SRAM and we hand each
// dirty rectangle off to lcd_panel::flush_area(), which writes it into the
// PSRAM framebuffer (the LCD_CAM peripheral continuously DMAs the framebuffer
// out to the panel).
//
// Buffer height = 60 lines. 800 * 60 * 2 = 96,000 bytes in internal SRAM.
// Halves the number of flush passes per full-screen render (8 vs 16).
// BLE stack (M5) will consume ~40 KB; at 127 KB / 328 KB total static SRAM
// there is still ~200 KB headroom for the stack and runtime allocations.
constexpr uint16_t DRAW_BUF_LINES = 60;
constexpr size_t   DRAW_BUF_PX    = 800 * DRAW_BUF_LINES;

static lv_disp_draw_buf_t draw_buf_dsc;
static lv_disp_drv_t      disp_drv;
static lv_color_t         draw_buf[DRAW_BUF_PX];   // ~64 KB internal RAM

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    // lv_color_t is RGB565 when LV_COLOR_DEPTH=16 and LV_COLOR_16_SWAP=0,
    // so the in-memory layout is already a `uint16_t*` of native pixels.
    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(color_p);
    lcd_panel::flush_area(area->x1, area->y1, area->x2, area->y2, pixels);
    lv_disp_flush_ready(drv);
}

} // namespace

namespace lvgl_display {

void init() {
    lv_disp_draw_buf_init(&draw_buf_dsc, draw_buf, nullptr, DRAW_BUF_PX);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = lcd_panel::width();
    disp_drv.ver_res  = lcd_panel::height();
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf_dsc;
    // Partial render mode: LVGL paints into the small buf, we flush rects.
    disp_drv.full_refresh        = 0;
    disp_drv.direct_mode         = 0;

    lv_disp_drv_register(&disp_drv);

    Serial.printf("[lvgl] display registered %ux%u draw_buf=%uKB\n",
                  disp_drv.hor_res, disp_drv.ver_res,
                  (unsigned)(sizeof(draw_buf) / 1024));
}

} // namespace lvgl_display
