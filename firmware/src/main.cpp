// Ori firmware entry point.
//
// Boot order is deliberate:
//
//   1. nvs::init()         — opens Preferences; cheap.
//   2. ble_core::init()    — NimBLEDevice::init() writes BLE config and IRK to
//                            flash, which temporarily disables the CPU cache.
//                            This MUST happen before lcd_panel::init() starts
//                            the RGB DMA — the DMA ISR lives in flash and will
//                            fault if the cache is toggled underneath it.
//   3. touch::init()       — brings up the shared Wire bus, then ch422g::init()
//                            (which leaves EXIO2/LCD_BL low so the panel is
//                            dark), then issues the GT911 reset over EXIO1.
//   4. backlight::init()   — restores the saved on/off state over CH422G EXIO2.
//                            This is the "no white flash" guarantee: the
//                            backlight is brought from the deliberately-off
//                            state ch422g::init() left it in, straight to the
//                            user's saved state, before the panel framebuffer
//                            has any content. (Backlight is binary on this
//                            hardware — see backlight.cpp.)
//   5. lcd_panel::init()   — pulses LCD_RST via EXIO3, then brings up the
//                            RGB16 bus and clears the framebuffer to black.
//                            Safe now: all BLE flash writes are complete.
//   6. LVGL init + display driver + input adapter.
//   7. Long-press threshold — set to 3000 ms per memory.md before any widget
//                             is created so the profile-photo and phone-disconnect
//                             long-press handlers fire at the right duration.
//   8. screen_manager::init() — calls state_machine::init() then evaluate()
//                               to pick the correct initial screen.

#include <Arduino.h>
#include <lvgl.h>

#include "backlight.h"
#include "ble/ble_core.h"
#include "gesture.h"
#include "lcd_panel.h"
#include "lvgl_display.h"
#include "lvgl_input.h"
#include "nvs_store.h"
#include "screen_manager.h"
#include "state_machine.h"
#include "touch_gt911.h"

// Print a one-line memory snapshot to Serial.
static void mem_snapshot(const char* tag) {
    Serial0.printf("[mem] %-22s  SRAM free=%6u  SRAM min=%6u  PSRAM free=%7u\n",
        tag,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void setup() {
    // Serial  = USB CDC  (requires host to be listening — unreliable for boot logs)
    // Serial0 = UART0    (GPIO43 TX / GPIO44 RX, always transmits — use this)
    Serial.begin(115200);   // keep alive for USB CDC upload compatibility
    Serial0.begin(115200);
    delay(50);
    Serial0.println();
    Serial0.println("[ori] boot");
    mem_snapshot("boot");

    nvs::init();

    // NimBLE writes BLE config + IRK to flash here — cache temporarily disabled.
    // Must complete before lcd_panel::init() starts the RGB DMA.
    ble_core::init();
    mem_snapshot("after ble_core");

    touch::init();
    mem_snapshot("after touch+ch422g");

    backlight::init();

    lcd_panel::init();
    mem_snapshot("after lcd_panel");

    lv_init();
    mem_snapshot("after lv_init");

    lvgl_display::init();
    mem_snapshot("after lvgl_display");

    lvgl_input::init();

    // Long-press threshold — 3000 ms per memory.md (factory reset trigger,
    // re-pair phone trigger). In LVGL 8.x the value lives on the driver struct:
    //   indev->driver->long_press_time
    // Set it on every registered input device before screen_manager::init()
    // so all long-press callbacks fire at the right duration.
    {
        lv_indev_t* indev = lv_indev_get_next(nullptr);
        while (indev) {
            if (indev->driver) {
                indev->driver->long_press_time = 3000;
            }
            indev = lv_indev_get_next(indev);
        }
        Serial.println("[ori] long press threshold set to 3000 ms");
    }

    screen_manager::init();
    mem_snapshot("after screen_manager");

    Serial0.println("[ori] setup complete");
    Serial0.println("[mem] --- SRAM 'min' = watermark (lowest ever seen since boot) ---");
}

void loop() {
    TouchPoint points[5];
    uint8_t n = touch::poll(points);

    gesture::update(points, n);
    nvs::tick();

    // ------------------------------------------------------------------
    // LVGL_TICK_HOOK
    //   esp32-lvgl agent: call lv_timer_handler() and forward single-touch
    //   into LVGL HERE. Suspend single-touch when n >= 2 (the backlight
    //   swipe gesture is active). Use touch::poll output directly —
    //   gesture has already consumed what it needs.
    //
    // Tick: LV_TICK_CUSTOM in lv_conf.h reads millis() directly, so we
    // don't need lv_tick_inc(). The touch::poll() above is the single
    // hardware read per loop — its result is forwarded to LVGL via feed(),
    // which caches it for the input device's read_cb. The 2+ touch suspend
    // rule is enforced inside lvgl_input.cpp.
    // ------------------------------------------------------------------
    lvgl_input::feed(points, n);
    screen_manager::poll_serial();  // before timer so a screen change renders this iteration
    ble_core::poll();               // drain BLE events → LVGL calls (M5)
    lv_timer_handler();

    // 100 Hz loop. LVGL refreshes at 60 Hz, touch is INT-driven, nothing
    // else benefits from a faster tick. Keeps touch-to-display latency
    // under 10 ms and halves CPU wakeups vs a 5 ms sleep.
    delay(10);
}
