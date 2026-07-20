// Ori firmware entry point.
//
// Boot order is deliberate:
//
//   1. nvs::init()         — opens Preferences; cheap.
//   1b. factory_info::init() — read-only load from the separate "factory" NVS
//                              partition (serial number + manufacture date,
//                              provisioning.md). Never written by firmware.
//   2. touch::init()       — brings up the shared Wire bus, then ch422g::init()
//                            (which leaves EXIO2/LCD_BL low so the panel is
//                            dark), then issues the GT911 reset over EXIO1.
//   3. backlight::init()   — turns the backlight ON (always-on policy).
//                            ch422g::init() leaves EXIO2 LOW so the panel is
//                            dark until this call, preventing a white flash
//                            before the framebuffer has any content.
//   4. lcd_panel::init()   — pulses LCD_RST via EXIO3, then brings up the
//                            RGB16 bus and clears the framebuffer to black.
//   5. LVGL init + display driver.
//   6. Boot splash — load screen_boot_splash (the Ori wordmark) and force ONE
//                    explicit lv_timer_handler() call to flush it to the
//                    physical LCD right now. Everything from here through
//                    ble_manager::init() below is blocking and runs entirely
//                    inside setup(), before loop() ever calls
//                    lv_timer_handler() on its own — without this the panel
//                    would otherwise sit on a plain black screen for that
//                    whole window. See screen_boot_splash.h.
//   7. Input adapter init.
//   8. Long-press threshold — set to 3000 ms per memory.md before any widget
//                             is created so the profile-photo and phone-disconnect
//                             long-press handlers fire at the right duration.
//   9. screen_manager::init() — calls state_machine::init() then evaluate() to
//                               pick the real initial screen. This only builds
//                               the screen object tree (no render/flush yet —
//                               that's still gated behind loop()), so the
//                               splash from step 6 stays the only thing
//                               actually visible until loop() begins; the
//                               eventual lv_scr_load_anim(..., auto_del=true)
//                               that swaps it in also deletes the splash.
//  10. ota_receiver::init()   — prepare USB CDC OTA framing state machine.
//  11. ble_manager::init()    — NimBLE stack, GATT server, ANCS client, advertising.
//                               Must be after screen_manager so passkey modal is ready.

#include <Arduino.h>
#include "ori_log.h"
#include <lvgl.h>

#include "app_state.h"
#include "backlight.h"
#include "ble/ble_manager.h"
#include "factory_info.h"
#include "lcd_panel.h"
#include "lvgl_display.h"
#include "lvgl_input.h"
#include "nvs_store.h"
#include "nvs_sync.h"
#include "ota_receiver.h"
#include "assets/profile_placeholder.h"
#include "assets/time_off_placeholder.h"
#include "photo_cache.h"
#include "screen_manager.h"
#include "time_format.h"
#include "holiday_data.h"
#include "screens/screen_boot_splash.h"
#include "state_machine.h"
#include "theme.h"
#include "widgets/widget_profile_card.h"
#include "touch_gt911.h"

// Arduino-ESP32 defaults the loop task to an 8192-byte stack. ble_manager::poll()
// calls gatt_server::set_device_status() -> NimBLECharacteristic::notify(), which
// can synchronously walk into NimBLE's ble_store_config_persist_cccds() -> NVS
// flash write when a bonded peer's CCCD is dirty after reconnect — the same call
// chain that overflowed the *NimBLE host task*'s 4096-byte stack with two bonded
// peers reconnecting at once (see CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE in
// platformio.ini). That fix only widened the host task; this widens the loop
// task to the same 16384 bytes for the identical worst case reached via notify().
size_t getArduinoLoopTaskStackSize(void) {
    return 16384;
}

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
    //
    // Enlarge the CDC RX ring BEFORE begin(). The default is 256 bytes, which
    // overflows during a USB CDC OTA: the main loop only drains serial inside
    // ota_receiver::poll() and there is a multi-ms gap each iteration (LVGL
    // render + delay(10)) where incoming DATA piles up. A small ring drops
    // bytes mid-frame → the OTA frame parser desyncs → transfer stalls. 16 KB
    // absorbs the per-iteration burst; USB endpoint NAK back-pressures the host
    // when even that fills, so no bytes are lost. (~16 KB SRAM; plenty free.)
    Serial.setRxBufferSize(32768);
    Serial.begin(115200);
    delay(50);
    LOG("\n");
    LOG("[ori] boot\n");
    mem_snapshot("boot");

    nvs::init();
    factory_info::init();  // read-only load from the separate "factory" NVS
                            // partition — serial number + manufacture date
    time_format::init();  // load 12/24-hour preference before any clock renders
    holiday_data::init(); // load holiday country + cached lunar (Tet) dates from NVS
    app_state::init();  // PSRAM-allocate the ANCS notification-detail store
    // Prime the BLE bond-address RAM cache now, before state_machine::init()
    // (reads the iPhone slot) and ble_manager::init() (starts the BLE stack).
    // After this, bond reads never open NVS — so the NimBLE host-task callbacks
    // can't race the main task's NVS access (the power-cycle reconnect crash).
    ble_manager::prime_bond_cache();
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

    // Boot splash — see the boot-order comment above. One explicit
    // lv_timer_handler() call forces the render + LCD_CAM flush right now
    // (the same render path flush_area()/esp_cache_msync() that loop() uses
    // every iteration) instead of waiting for loop() to get around to it
    // after the rest of (blocking) setup() has already run.
    lv_scr_load_anim(screen_boot_splash::create(), LV_SCR_LOAD_ANIM_NONE, 0, 0, /*auto_del=*/true);
    lv_timer_handler();
    mem_snapshot("after boot splash");

    lvgl_input::init();

    // Long-press threshold — 3000 ms per memory.md (factory reset trigger,
    // re-pair phone trigger). In LVGL 9 the value is set via API on the
    // lv_indev_t object directly (the driver struct no longer exists).
    lv_indev_set_long_press_time(lvgl_input::get(), 3000);
    LOG("[ori] long press threshold set to 3000 ms\n");

    // Build the color-emoji fallback and attach it to the user-text fonts before
    // any runtime screen (or ANCS text) is built. Needs LVGL initialized.
    theme::init_emoji_fallback();

    screen_manager::init();
    mem_snapshot("after screen_manager");

    // Load cached profile text from NVS and photos from LittleFS before first screen draw.
    {
        // Buffers sized for the 32/32/32/16-char field limits at worst-case
        // UTF-8 (3 bytes/char for the scripts we ship, e.g. Vietnamese).
        char name[97] = {}, title[97] = {}, email[129] = {}, phone[33] = {};
        if (nvs_sync::load_profile(name, sizeof(name), title, sizeof(title),
                                   email, sizeof(email), phone, sizeof(phone))) {
            widget_profile_card::set_profile(name, title, email, phone);
            LOG("[boot] profile loaded: name=%s title=%s\n", name, title);
        }
    }
    // Shortcut Config is RAM-only (ble-protocol.md §6.0, like Meeting List) —
    // no NVS load here. app_state's compiled-in defaults (vol-mute/mic-mute/
    // screenshot) hold until Orion's next sync re-pushes it.
    // Mount LittleFS before any photo_cache call — user photos live there.
    photo_cache::mount_fs();
    // Placeholders (compiled into firmware flash) decoded first so init() can
    // fall back to them immediately if no user photo exists on LittleFS yet.
    photo_cache::init_profile_placeholder(profile_placeholder_jpg, profile_placeholder_jpg_len);
    photo_cache::init_time_off_placeholder(time_off_placeholder_jpg, time_off_placeholder_jpg_len);
    // Load user photos from LittleFS (persists across firmware updates).
    photo_cache::init();
    photo_cache::init_time_off();
    mem_snapshot("after photo_cache");

    // Meetings are RAM-only (see state_machine::set_meetings_cbor) — nothing to
    // load from NVS at boot. After a power cycle the list is empty until Orion
    // reconnects and re-pushes it.

    // OTA receiver + BLE stack.
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

    // Drain BLE event queue before LVGL so BLE-driven state changes are
    // reflected in the current frame. ota_receiver polls USB CDC for OTA frames.
    ble_manager::poll();
    ota_receiver::poll();
    state_machine::poll();  // drain deferred NVS writes before LVGL renders

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
