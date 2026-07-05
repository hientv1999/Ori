// Ori LVGL desktop simulator — renders every screen to a BMP file.
//
// Pipeline:
//   1. Initialise LVGL with a software framebuffer at 800x480 RGB565.
//   2. For each screen, call its factory, run lv_timer_handler() a few
//      times so layout + initial animation frames settle, then snapshot the
//      framebuffer to a BMP file in sim/screenshots/.
//   3. Repeat for every screen + variant listed in render_all().
//
// This binary deliberately links only the LVGL-pure files from firmware/:
//   theme, mock_data, screen_manager (unused here but harmless), the three
//   widgets, and every screen.cpp. Hardware modules (backlight, touch_gt911,
//   nvs_store, io_expander_ch422g, lcd_panel, lvgl_display, lvgl_input,
//   firmware/src/main.cpp) are NOT linked — those depend on Arduino headers
//   we don't have on desktop.

#include "arduino_shim.h"

#include "lvgl.h"

#include "mock_data.h"
#include "theme.h"
#include "widgets/widget_status_bar.h"
#include "widgets/widget_profile_card.h"

#include "screens/screen_keyboard_mode.h"
#include "screens/screen_meeting_list.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"
#include "screens/screen_no_meetings.h"
#include "screens/screen_clock.h"
#include "screens/screen_time_off.h"
#include "screens/screen_repair_phone.h"
#include "screens/screen_reconnect_syncing.h"
#include "screens/screen_ota_updating.h"
#include "screens/screen_setup.h"
#include "screens/modal_countdown.h"
#include "screens/modal_factory_reset.h"
#include "screens/modal_profile.h"
#include "screens/modal_unpair_phone.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Framebuffer + LVGL display driver
// ---------------------------------------------------------------------------

constexpr int W = 800;
constexpr int H = 480;

// Small partial-render buffer — matches the firmware's strategy
// (30-line strip in fast SRAM). Sized identically for fidelity.
static lv_color_t draw_buf[W * 30];

// Full-screen accumulator. The flush callback writes dirty rects into this,
// and at snapshot time we serialise the whole thing to BMP.
static lv_color_t fb_full[W * H];

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    int aw = area->x2 - area->x1 + 1;
    for (int y = 0; y <= area->y2 - area->y1; ++y) {
        lv_color_t* src = color_p + y * aw;
        lv_color_t* dst = fb_full + (area->y1 + y) * W + area->x1;
        std::memcpy(dst, src, aw * sizeof(lv_color_t));
    }
    lv_disp_flush_ready(drv);
}

static void init_lvgl_display() {
    static lv_disp_draw_buf_t draw_buf_dsc;
    lv_disp_draw_buf_init(&draw_buf_dsc, draw_buf, nullptr, W * 30);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = W;
    disp_drv.ver_res = H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf_dsc;
    disp_drv.full_refresh = 0;
    disp_drv.direct_mode = 0;
    lv_disp_drv_register(&disp_drv);
}

// ---------------------------------------------------------------------------
// BMP writer
// ---------------------------------------------------------------------------
//
// Tiny 24-bit BGR Windows BMP. No deps. Row stride = W*3 = 2400 (already
// 4-byte aligned for W=800, so no padding required).

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t bfType;       // 'BM'
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
};

struct BmpInfoHeader {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};
#pragma pack(pop)

static inline uint8_t expand5(uint8_t v) { return (v << 3) | (v >> 2); }
static inline uint8_t expand6(uint8_t v) { return (v << 2) | (v >> 4); }

static bool save_bmp(const char* path) {
    constexpr uint32_t stride = W * 3;
    constexpr uint32_t pixel_bytes = stride * H;
    constexpr uint32_t file_bytes  = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) + pixel_bytes;

    BmpFileHeader fh{};
    fh.bfType     = 0x4D42;  // 'BM'
    fh.bfSize     = file_bytes;
    fh.bfOffBits  = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);

    BmpInfoHeader ih{};
    ih.biSize       = sizeof(BmpInfoHeader);
    ih.biWidth      = W;
    ih.biHeight     = H;          // positive = bottom-up, the BMP default
    ih.biPlanes     = 1;
    ih.biBitCount   = 24;
    ih.biCompression= 0;
    ih.biSizeImage  = pixel_bytes;

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);

    // BMP rows are stored bottom-up. Allocate one row buffer and stream.
    static uint8_t row[stride];
    for (int y = H - 1; y >= 0; --y) {
        const lv_color_t* src = fb_full + y * W;
        for (int x = 0; x < W; ++x) {
            // LVGL RGB565: low 5 bits = blue, mid 6 = green, top 5 = red.
            // Expand each to 8-bit by bit-replication; write as BGR.
            lv_color_t c = src[x];
            uint8_t r5 = c.ch.red;
            uint8_t g6 = c.ch.green;
            uint8_t b5 = c.ch.blue;
            row[x * 3 + 0] = expand5(b5);
            row[x * 3 + 1] = expand6(g6);
            row[x * 3 + 2] = expand5(r5);
        }
        std::fwrite(row, 1, stride, f);
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Snapshot helper — load screen, settle layout, save
// ---------------------------------------------------------------------------

// Tracks the screen we loaded LAST time so we can delete it AFTER loading
// the next one. Deleting the currently-active screen leaves LVGL with a
// dangling default-screen pointer → segfault on the next swap.
static lv_obj_t* g_prev_screen = nullptr;

static void snapshot(lv_obj_t* screen, const char* path) {
    lv_scr_load(screen);
    // Free the previous screen now that we've swapped off it. Safe because
    // `screen` is the new active one.
    if (g_prev_screen && g_prev_screen != screen) {
        lv_obj_del(g_prev_screen);
    }
    g_prev_screen = screen;

    // Give layout a few timer ticks to settle (fonts, flex, scroll geometry).
    for (int i = 0; i < 10; ++i) lv_timer_handler();

    // Force LVGL to consider the entire display dirty and paint everything
    // on the very next refresh. Without this, LVGL's incremental diff
    // tracker can leave most of the framebuffer holding stale content from
    // the previous screen.
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    if (save_bmp(path)) {
        std::printf("[sim] wrote %s\n", path);
    } else {
        std::printf("[sim] FAILED to write %s\n", path);
    }
}

// ---------------------------------------------------------------------------
// Render catalogue — one call per screen / variant
// ---------------------------------------------------------------------------

static void render_all() {
    // --- Diagnostic (screen_color_test removed) --------------------------

    // --- Runtime screens -----------------------------------------------
    snapshot(screen_meeting_list::create(mock_data::meetings(), false),
             "screenshots/01_meeting_list.bmp");

    snapshot(screen_meeting_list::create(mock_data::meetings_overlap(), false),
             "screenshots/02_meeting_list_overlap.bmp");

    snapshot(screen_meeting_list::create(mock_data::meetings_long_title(), false),
             "screenshots/03_meeting_list_long_title.bmp");

    snapshot(screen_meeting_list::create(mock_data::meetings_overlap_long(), false),
             "screenshots/04_meeting_list_overlap_long.bmp");

    snapshot(screen_meeting_list::create(mock_data::meetings_long_list(), false),
             "screenshots/05_meeting_list_scrollable.bmp");

    snapshot(screen_meeting_list::create(mock_data::meetings(), true),
             "screenshots/06_meeting_list_cached.bmp");

    // Phone-disconnected variant — exercises the new phone-broken glyph and
    // the rightmost-mode-toggle ordering rule.
    {
        mock_data::AncsConfig off = { { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }, 0, false };
        mock_data::set_ancs_config(off);
        snapshot(screen_meeting_list::create(mock_data::meetings(), false),
                 "screenshots/06b_meeting_list_phone_off.bmp");
        // Restore default ANCS for subsequent screens.
        mock_data::AncsConfig def = { { "gmail", "messenger", "instagram", nullptr, nullptr, nullptr }, 3, true };
        mock_data::set_ancs_config(def);
    }

    snapshot(screen_no_meetings::create(),       "screenshots/07_no_meetings.bmp");
    snapshot(screen_clock::create(),             "screenshots/08_clock.bmp");
    snapshot(screen_time_off::create(),          "screenshots/09_time_off.bmp");

    // --- Modals (overlay a base screen) --------------------------------
    {
        lv_obj_t* base = screen_meeting_list::create(mock_data::meetings(), false);
        lv_scr_load(base);
        if (g_prev_screen && g_prev_screen != base) lv_obj_del(g_prev_screen);
        g_prev_screen = base;
        for (int i = 0; i < 10; ++i) lv_timer_handler();
        // Pass nullptr for ref_photo — no live profile card to pin against in the sim.
        modal_profile::create(base, nullptr);
        for (int i = 0; i < 10; ++i) lv_timer_handler();
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        save_bmp("screenshots/10b_modal_profile.bmp");
        std::printf("[sim] wrote screenshots/10b_modal_profile.bmp\n");
    }

    {
        lv_obj_t* base = screen_meeting_list::create(mock_data::meetings(), false);
        lv_scr_load(base);
        if (g_prev_screen && g_prev_screen != base) lv_obj_del(g_prev_screen);
        g_prev_screen = base;
        for (int i = 0; i < 10; ++i) lv_timer_handler();
        modal_countdown::create(base,
            "Industrial design review",
            "Starts at 10:30 · Studio",
            300);
        for (int i = 0; i < 10; ++i) lv_timer_handler();
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        save_bmp("screenshots/10_modal_countdown.bmp");
        std::printf("[sim] wrote screenshots/10_modal_countdown.bmp\n");
    }

    {
        lv_obj_t* base = screen_meeting_list::create(mock_data::meetings(), false);
        lv_scr_load(base);
        if (g_prev_screen && g_prev_screen != base) lv_obj_del(g_prev_screen);
        g_prev_screen = base;
        for (int i = 0; i < 10; ++i) lv_timer_handler();
        modal_factory_reset::create(base);
        for (int i = 0; i < 10; ++i) lv_timer_handler();
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        save_bmp("screenshots/11_modal_factory_reset.bmp");
        std::printf("[sim] wrote screenshots/11_modal_factory_reset.bmp\n");
    }

    {
        lv_obj_t* base = screen_meeting_list::create(mock_data::meetings(), false);
        lv_scr_load(base);
        if (g_prev_screen && g_prev_screen != base) lv_obj_del(g_prev_screen);
        g_prev_screen = base;
        for (int i = 0; i < 10; ++i) lv_timer_handler();
        modal_unpair_phone::create(base);
        for (int i = 0; i < 10; ++i) lv_timer_handler();
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        save_bmp("screenshots/11b_modal_unpair_phone.bmp");
        std::printf("[sim] wrote screenshots/11b_modal_unpair_phone.bmp\n");
    }

    // --- Setup flow ----------------------------------------------------
    snapshot(screen_setup::create(screen_setup::Step::Welcome),
             "screenshots/12_setup_welcome.bmp");

    snapshot(screen_setup::create(screen_setup::Step::Install),
             "screenshots/13_setup_step1_install.bmp");

    snapshot(screen_setup::create(screen_setup::Step::Pairing),
             "screenshots/14_setup_step2_pairing.bmp");

    {
        lv_obj_t* setup = screen_setup::create(screen_setup::Step::Pairing);
        screen_setup::show_passkey_modal(setup);
        lv_scr_load(setup);
        if (g_prev_screen && g_prev_screen != setup) lv_obj_del(g_prev_screen);
        g_prev_screen = setup;
        for (int i = 0; i < 10; ++i) lv_timer_handler();
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        save_bmp("screenshots/15_setup_step2_passkey.bmp");
        std::printf("[sim] wrote screenshots/15_setup_step2_passkey.bmp\n");
    }

    snapshot(screen_setup::create(screen_setup::Step::Orioning),
             "screenshots/16_setup_step3_orioning.bmp");

    snapshot(screen_setup::create(screen_setup::Step::PhonePairing),
             "screenshots/17_setup_step4_phone.bmp");

    snapshot(screen_setup::create(screen_setup::Step::Complete),
             "screenshots/18_setup_complete.bmp");

    // --- Runtime overlays ---------------------------------------------
    snapshot(screen_repair_phone::create(),       "screenshots/19_repair_phone.bmp");
    snapshot(screen_reconnect_syncing::create(),  "screenshots/20_reconnect_syncing.bmp");
    snapshot(screen_ota_updating::create(),       "screenshots/21_ota_updating.bmp");

    // --- Controls mode (keyboard mode) — playing + paused + empty -----
    // Mode-toggle should appear on the status bar with the calendar icon
    // (we're already in Controls, so tapping returns to calendar).
    widget_status_bar::set_default_mode(widget_status_bar::Mode::Keyboard);
    widget_status_bar::set_default_pc_connected(true);
    widget_profile_card::set_default_presence(widget_profile_card::Presence::Available);
    mock_data::set_media_playing(true);
    snapshot(screen_keyboard_mode::create(),      "screenshots/22_controls_playing.bmp");
    mock_data::set_media_playing(false);
    snapshot(screen_keyboard_mode::create(),      "screenshots/23_controls_paused.bmp");

    // --- Teams-presence demo (border colours) -------------------------
    // Re-render the meeting list with each presence applied.
    widget_status_bar::set_default_mode(widget_status_bar::Mode::Calendar);
    widget_profile_card::set_default_presence(widget_profile_card::Presence::Available);
    snapshot(screen_meeting_list::create(mock_data::meetings(), false),
             "screenshots/24_presence_available.bmp");
    widget_profile_card::set_default_presence(widget_profile_card::Presence::Busy);
    snapshot(screen_meeting_list::create(mock_data::meetings(), false),
             "screenshots/25_presence_busy.bmp");
    widget_profile_card::set_default_presence(widget_profile_card::Presence::Away);
    snapshot(screen_meeting_list::create(mock_data::meetings(), false),
             "screenshots/26_presence_away.bmp");
    widget_profile_card::set_default_presence(widget_profile_card::Presence::Offline);
    snapshot(screen_meeting_list::create(mock_data::meetings(), false),
             "screenshots/27_presence_offline.bmp");

    // --- PC offline — mode-toggle disappears + border forces to grey --
    widget_status_bar::set_default_pc_connected(false);
    widget_profile_card::set_default_presence(widget_profile_card::Presence::Offline);
    snapshot(screen_meeting_list::create(mock_data::meetings(), true),
             "screenshots/28_pc_offline.bmp");
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered so we see crashes
    std::printf("[sim] Ori LVGL screenshot renderer starting (LV_MEM_SIZE=%u)\n",
                (unsigned)LV_MEM_SIZE);
    lv_init();
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    std::printf("[sim] runtime LVGL pool total_size=%u\n", (unsigned)mon.total_size);
    init_lvgl_display();
    render_all();
    std::printf("[sim] done.\n");
    return 0;
}
