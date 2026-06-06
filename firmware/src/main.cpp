// Ori firmware entry point.
//
// Boot order is deliberate:
//
//   1. nvs::init()         — opens Preferences; cheap.
//   2. touch::init()       — brings up the shared Wire bus, then ch422g::init()
//                            (which leaves EXIO2/LCD_BL low so the panel is
//                            dark), then issues the GT911 reset over EXIO1.
//   3. backlight::init()   — turns the backlight ON (always-on policy).
//                            ch422g::init() leaves EXIO2 LOW so the panel is
//                            dark until this call, preventing a white flash
//                            before the framebuffer has any content.
//   4. lcd_panel::init()   — pulses LCD_RST via EXIO3, then brings up the
//                            RGB16 bus and clears the framebuffer to black.
//   5. LVGL init + display driver + input adapter.
//   6. Long-press threshold — set to 3000 ms per memory.md before any widget
//                             is created so the profile-photo and phone-disconnect
//                             long-press handlers fire at the right duration.
//   7. screen_manager::init() — calls state_machine::init() then evaluate()
//                               to pick the correct initial screen.
//   8. ota_receiver::init()   — prepare USB CDC OTA framing state machine.
//   9. ble_manager::init()    — NimBLE stack, GATT server, ANCS client, advertising.
//                               Must be after screen_manager so passkey modal is ready.

#include <Arduino.h>
#include "ori_log.h"
#include <lvgl.h>

#include "backlight.h"
#include "ble/ble_manager.h"
#include "lcd_panel.h"
#include "lvgl_display.h"
#include "lvgl_input.h"
#include "nvs_store.h"
#include "ota_receiver.h"
#include "photo_cache.h"
#include "screen_manager.h"
#include "touch_gt911.h"

// Print a one-line memory snapshot to Serial.
static void mem_snapshot(const char* tag) {
    LOG("[mem] %-22s  SRAM free=%6u  SRAM min=%6u  PSRAM free=%7u\n",
        tag,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void setup() {
    // Serial  = USB CDC  (requires host to be listening — unreliable for boot logs)
    // Serial0 = UART0    (GPIO43 TX / GPIO44 RX, always transmits — use this)
    Serial.begin(115200);   // keep alive for USB CDC upload compatibility
    Serial.begin(115200);
    delay(50);
    LOG("\n");
    LOG("[ori] boot\n");
    mem_snapshot("boot");

    nvs::init();
    touch::init();
    mem_snapshot("after touch+ch422g");

    backlight::init();

    lcd_panel::init();
    mem_snapshot("after lcd_panel");

    lv_init();
    // LVGL 9: provide the tick source via callback instead of LV_TICK_CUSTOM.
    // millis() is uint32_t on ESP32, matching lv_tick_get_cb_t.
    lv_tick_set_cb((uint32_t(*)(void))millis);
    mem_snapshot("after lv_init");

    lvgl_display::init();
    mem_snapshot("after lvgl_display");

    lvgl_input::init();

    // Long-press threshold — 3000 ms per memory.md (factory reset trigger,
    // re-pair phone trigger). In LVGL 9 the value is set via API on the
    // lv_indev_t object directly (the driver struct no longer exists).
    lv_indev_set_long_press_time(lvgl_input::get(), 3000);
    LOG("[ori] long press threshold set to 3000 ms\n");

    screen_manager::init();
    mem_snapshot("after screen_manager");

    // Load cached photos from NVS and decode to PSRAM before first screen draw.
    photo_cache::init();
    photo_cache::init_pto();
    mem_snapshot("after photo_cache");

    // M5: OTA receiver + BLE stack.
    // ota_receiver must be initialised before ble_manager because it sets up
    // the USB CDC framing parser which runs independently of BLE.
    ota_receiver::init();

    // BLE must be last because ble_manager::init() starts advertising immediately,
    // and passkey modal events need screen_manager to be running.
    ble_manager::init();
    mem_snapshot("after ble_manager");

    LOG("[ori] setup complete\n");
    LOG("[mem] --- SRAM 'min' = watermark (lowest ever seen since boot) ---\n");
}

void loop() {
    TouchPoint points[5];
    uint8_t n = touch::poll(points);

    nvs::tick();

    // M5: drain BLE event queue before LVGL so BLE-driven state changes are
    // reflected in the current frame. ota_receiver polls USB CDC for OTA frames.
    ble_manager::poll();
    ota_receiver::poll();

    // ------------------------------------------------------------------
    // LVGL_TICK_HOOK
    //   esp32-lvgl agent: call lv_timer_handler() and forward touch into
    //   LVGL HERE. touch::poll() is the single hardware read per loop —
    //   its result is forwarded via feed() for the input device's read_cb.
    // ------------------------------------------------------------------
    lvgl_input::feed(points, n);
    screen_manager::poll_serial();  // before timer so a screen change renders this iteration
    lv_timer_handler();

    // 100 Hz loop. LVGL refreshes at 60 Hz, touch is INT-driven, nothing
    // else benefits from a faster tick. Keeps touch-to-display latency
    // under 10 ms and halves CPU wakeups vs a 5 ms sleep.
    delay(10);
}
