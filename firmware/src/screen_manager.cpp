#include "screen_manager.h"

#include <Arduino.h>
#include "ori_log.h"
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "factory_reset.h"
#include "app_state.h"
#include "holiday_data.h"
#include "ota_receiver.h"
#include "state_machine.h"
#include "time_format.h"
#include "screens/modal_countdown.h"
#include "screens/modal_factory_reset.h"
#include "screens/modal_incoming_call.h"
#include "screens/modal_unpair_phone.h"
#include "screens/screen_calendar.h"
#include "screens/screen_clock.h"
#include "screens/screen_clock_analog.h"
#include "screens/screen_media_mode.h"
#include "screens/screen_no_meetings.h"
#include "screens/screen_ota_updating.h"
#include "screens/screen_time_off.h"
#include "screens/screen_reconnect_syncing.h"
#include "screens/screen_setup.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Ori — screen manager.
//
// The state machine (state_machine.cpp) owns all screen transitions.
// This file initialises the state machine on boot and routes serial input
// to the debug screen cycler when ORI_DEBUG_SERIAL is defined.

namespace {

// Runtime state for the debug cycler. State machine is authoritative;
// these are only used here when cycling screens manually.
//
// Both default to the real "nothing connected yet" boot state — this same
// pair also feeds apply_default_widget_state(), which apply_state_defaults()
// (called unconditionally from screen_manager::init(), not gated by
// ORI_DEBUG_SERIAL) uses to set the profile-photo border color on every real
// boot. Defaulting these to Connected/true previously made the border flash
// green at cold boot before BLE had done anything, contradicting
// widget_profile_card.h's documented "defaults to Disconnected so the device
// never flashes a stale Connected green" intent. The 'P'/'X' debug keys still
// cycle these up to Connected/true for hand-testing after boot.
widget_profile_card::ConnStatus g_conn_status  = widget_profile_card::ConnStatus::Disconnected;
bool                          g_pc_connected = false;
widget_status_bar::Mode       g_status_mode  = widget_status_bar::Mode::Calendar;

// 'W'/'N' debug-cycler state — see debug_handle_key() cases 'W'/'N'. Starts
// hidden (g_weather_shown = false) to mirror the real device's pre-first-sync
// default (widget_profile_card's own g_default_weather_visible).
//
// Fixed ordered (condition, intensity) presets 'W' cycles through — every
// intensity level a condition can actually report, skipping the intensity
// axis (None) for conditions that don't have one. 'N' independently toggles
// day/night for whichever preset is currently selected, re-applying the same
// condition/intensity with the flipped is_night.
struct WeatherPreset {
    widget_profile_card::WeatherCondition cond;
    widget_profile_card::WeatherIntensity intensity;
};
constexpr WeatherPreset kWeatherPresets[] = {
    { widget_profile_card::WeatherCondition::Clear,        widget_profile_card::WeatherIntensity::None },
    { widget_profile_card::WeatherCondition::PartlyCloudy, widget_profile_card::WeatherIntensity::None },
    { widget_profile_card::WeatherCondition::Cloudy,       widget_profile_card::WeatherIntensity::None },
    { widget_profile_card::WeatherCondition::Rain,         widget_profile_card::WeatherIntensity::Light },
    { widget_profile_card::WeatherCondition::Rain,         widget_profile_card::WeatherIntensity::Moderate },
    { widget_profile_card::WeatherCondition::Rain,         widget_profile_card::WeatherIntensity::Heavy },
    { widget_profile_card::WeatherCondition::Thunderstorm, widget_profile_card::WeatherIntensity::Light },
    { widget_profile_card::WeatherCondition::Thunderstorm, widget_profile_card::WeatherIntensity::Moderate },
    { widget_profile_card::WeatherCondition::Thunderstorm, widget_profile_card::WeatherIntensity::Heavy },
    { widget_profile_card::WeatherCondition::Snow,         widget_profile_card::WeatherIntensity::Light },
    { widget_profile_card::WeatherCondition::Snow,         widget_profile_card::WeatherIntensity::Moderate },
    { widget_profile_card::WeatherCondition::Snow,         widget_profile_card::WeatherIntensity::Heavy },
    { widget_profile_card::WeatherCondition::Fog,          widget_profile_card::WeatherIntensity::Light },
    { widget_profile_card::WeatherCondition::Fog,          widget_profile_card::WeatherIntensity::Heavy },
};
constexpr size_t kWeatherPresetCount = sizeof(kWeatherPresets) / sizeof(kWeatherPresets[0]);

size_t g_weather_preset_idx = 0;
bool   g_weather_shown      = false;
bool   g_weather_is_night   = false;

// 'h' debug-cycler state — mirrors holiday_data::set_debug_override()'s own
// default (off), tracked here only so the key can toggle and log it (the
// module itself has no getter). No real holiday data source exists yet; this
// just lets the ring/text-color/subtitle treatment on the calendar month view
// ('v') and no-meetings glyph ('n') be hand-tested against illustrative demo
// dates (day 3 / day 20 of every month) without one. Press 'n' or 'v' again
// after toggling to see the effect — this key doesn't force-navigate there.
bool g_holiday_debug = false;

// ─────────────────────────────────────────────────────────────────────────────
// apply_state_defaults — set widget defaults to current runtime values before
// any screen create() call. Called from screen_manager::init() (once, at
// boot) and from the ORI_DEBUG_SERIAL 'R' key (re-run evaluate() by hand).
// state_machine.cpp does NOT call this — it has no reference to it; screen
// creation reads these widget-level defaults independently of the state
// machine's own AppState.
// ─────────────────────────────────────────────────────────────────────────────

// Connection-status/PC-link/mode fields shared by apply_state_defaults()
// (production) and debug_apply_defaults() (ORI_DEBUG_SERIAL cycler below) —
// the two differ only in what the mode-toggle button does, so only that
// callback is set by each caller individually.
void apply_default_widget_state() {
    auto eff = g_pc_connected ? g_conn_status : widget_profile_card::ConnStatus::Disconnected;
    widget_profile_card::set_default_conn_status(eff);
    widget_status_bar::set_default_pc_connected(g_pc_connected);
    widget_status_bar::set_default_mode(g_status_mode);
}

void apply_state_defaults() {
    apply_default_widget_state();
    widget_status_bar::set_default_mode_toggle_cb([]() {
        state_machine::on_mode_toggle();
    });
}

// ORI_DEBUG_SERIAL serial cycler — gated by ORI_DEBUG_SERIAL in platformio.ini.
// Each key loads a screen directly, bypassing the state machine.

#ifdef ORI_DEBUG_SERIAL

void debug_load(lv_obj_t* scr) {
    // auto_del=true: LVGL fires screen-unload events then immediately deletes
    // d->prev_scr and clears it to nullptr before lv_scr_load_anim() returns.
    // Calling lv_obj_delete(prev) after lv_scr_load() instead leaves d->prev_scr
    // dangling; on the next lv_timer_handler() LVGL dereferences it to fire
    // LV_EVENT_SCREEN_UNLOADED → LoadProhibited crash.
    //
    // lv_refr_now() is intentionally absent: poll_serial() is called before
    // lv_timer_handler() in loop(), so the new screen renders this iteration.
    // Forcing a synchronous render from here runs esp_cache_msync() outside
    // lv_timer_handler() (undefined LCD_CAM DMA ownership) and adds ~500 B of
    // extra caller frames, overflowing the 8 KB loopTask stack on heavy screens.
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, /*auto_del=*/true);
}

void debug_load_meeting_default() {
    debug_load(screen_no_meetings::create());
}

void debug_load_setup(screen_setup::Step st, bool with_passkey) {
lv_obj_t* s = screen_setup::create(st);
    debug_load(s);
    if (with_passkey) screen_setup::show_passkey_modal(s, 123456); // mock passkey for debug
}

void debug_apply_defaults() {
    apply_default_widget_state();
    widget_status_bar::set_default_mode_toggle_cb([]() {
        g_status_mode = (g_status_mode == widget_status_bar::Mode::Calendar)
                      ? widget_status_bar::Mode::Keyboard
                      : widget_status_bar::Mode::Calendar;
        debug_apply_defaults();
        if (g_status_mode == widget_status_bar::Mode::Keyboard) {
                debug_load(screen_media_mode::create());
        } else {
            debug_load_meeting_default();
        }
    });
}

void print_mem_stats() {
    auto sram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    auto sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    auto sram_lfb   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    auto sram_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    auto psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    auto psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    auto psram_lfb   = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    auto psram_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    LOG("\n");
    LOG("=== Memory stats ===\n");
    LOG("  SRAM   free %6u KB  used %6u KB  total %6u KB\n",
                  sram_free/1024, (sram_total-sram_free)/1024, sram_total/1024);
    LOG("         largest block %u KB   min free since boot %u KB\n",
                  sram_lfb/1024, sram_min/1024);
    LOG("  PSRAM  free %6u KB  used %6u KB  total %6u KB\n",
                  psram_free/1024, (psram_total-psram_free)/1024, psram_total/1024);
    LOG("         largest block %u KB   min free since boot %u KB\n",
                  psram_lfb/1024, psram_min/1024);
    LOG("====================\n");
    LOG("\n");
}

void print_keymap() {
    LOG("\n");
    LOG("=== Ori screen cycler (ORI_DEBUG_SERIAL) ===\n");
    LOG("  m   Meeting list\n");
    LOG("  n   No meetings today\n");
    LOG("  c   Digital clock (entered via time tap; sets clock-face pref -> Digital)\n");
    LOG("  a   Analog clock (entered via time tap; sets clock-face pref -> Analog)\n");
    LOG("  H   Toggle 12-/24-hour time format\n");
    LOG("  v   Calendar month view (entered via time long-press)\n");
    LOG("  p   Time Off scenic\n");
    LOG("  k   Media mode\n");
    LOG("  C   5-minute countdown modal\n");
    LOG("  P   Cycle connection-status border (Disconnected -> Syncing -> Connected)\n");
    LOG("  W   Cycle weather badge (Clear -> PartlyCloudy -> Cloudy -> Rain L/M/H ->\n");
    LOG("      Thunderstorm L/M/H -> Snow L/M/H -> Fog L/H, 72F)\n");
    LOG("  N   Toggle day/night for the current weather preset\n");
    LOG("  h   Toggle illustrative holiday demo data (day 3/20 of any month) —\n");
    LOG("      no real data source exists yet; affects 'n' and 'v' screens\n");
    LOG("  X   Toggle PC link state\n");
    LOG("  f   Factory reset modal\n");
    LOG("  F   FACTORY RESET — wipe NVS + BLE bonds + reboot (no confirmation)\n");
    LOG("  U   Unpair iPhone modal\n");
    LOG("  I   Incoming-call banner (mock)\n");
    LOG("  w   Setup — Welcome\n");
    LOG("  i   Setup — Step 1 Install Orion\n");
    LOG("  b   Setup — Step 2 Link Orion\n");
    LOG("  B   Setup — Step 2 Passkey Modal\n");
    LOG("  o   Setup — Step 2 Orioning Modal\n");
    LOG("  t   Setup — Step 3 iPhone pairing\n");
    LOG("  e   Setup — Complete\n");
    LOG("  x   Reconnect-Syncing overlay\n");
    LOG("  -- OTA update flow --\n");
    LOG("  u   OTA · Updating firmware (live ring)\n");
    LOG("  1   OTA · Installing (screen goes dark)\n");
    LOG("  2   OTA · Updated boot ack (Close)\n");
    LOG("  3   OTA · Update failed (Close)\n");
    LOG("  R   Re-run state machine evaluate() (real boot logic)\n");
    LOG("  M   Print SRAM / PSRAM free stats\n");
    LOG("  ?   Print this map\n");
    LOG("===============================================\n");
    LOG("\n");
}

// OTA flow nav callbacks — wired to the screens' buttons so a serial-loaded
// OTA screen can be clicked through by hand (does NOT touch the real OTA flow).
void dbg_ota_close(lv_event_t*)  { debug_load_meeting_default(); }

void debug_handle_key(char c) {
    debug_apply_defaults();
    switch (c) {
        case 'm':
            debug_load_meeting_default();
            break;
        case 'n':
                debug_load(screen_no_meetings::create());
            break;
        case 'c':
                state_machine::set_clock_face(0);
                debug_load(screen_clock::create());
            break;
        case 'a':
                state_machine::set_clock_face(1);
                debug_load(screen_clock_analog::create());
            break;
        case 'H': {
                uint8_t next = time_format::is_24h() ? 1 : 0;  // toggle 24h <-> 12h
                state_machine::set_time_format(next);
                LOG("[scr] time_format -> %s\n", next ? "12h" : "24h");
            break;
        }
        case 'v':
                screen_calendar::reset_view();
                debug_load(screen_calendar::create());
            break;
        case 'p':
                debug_load(screen_time_off::create());
            break;
        case 'C': {
            lv_obj_t* base = screen_no_meetings::create();
            debug_load(base);
            modal_countdown::create(base, "Industrial design review",
                "Priya Anand", "Studio 4 \xc2\xb7 3rd floor", 187);
            break;
        }
        case 'k': {
            g_status_mode = widget_status_bar::Mode::Keyboard;
            debug_apply_defaults();
            debug_load(screen_media_mode::create());
            break;
        }
        case 'P': {
            using CS = widget_profile_card::ConnStatus;
            switch (g_conn_status) {
                case CS::Disconnected: g_conn_status = CS::Syncing;      break;
                case CS::Syncing:      g_conn_status = CS::Connected;    break;
                default:                g_conn_status = CS::Disconnected; break;
            }
            auto eff = g_pc_connected ? g_conn_status : CS::Disconnected;
            widget_profile_card::set_default_conn_status(eff);
            LOG("[scr] conn_status -> %d\n", (int)eff);
            break;
        }
        case 'W': {
            if (g_weather_shown) {
                g_weather_preset_idx = (g_weather_preset_idx + 1) % kWeatherPresetCount;
            }
            g_weather_shown = true;
            const auto& preset = kWeatherPresets[g_weather_preset_idx];
            widget_profile_card::set_default_weather(
                preset.cond, 72, widget_profile_card::TemperatureUnit::Fahrenheit, true,
                g_weather_is_night, preset.intensity);
            LOG("[scr] weather -> condition=%d intensity=%d night=%d (72F)\n",
                (int)preset.cond, (int)preset.intensity, (int)g_weather_is_night);
            break;
        }
        case 'N': {
            g_weather_is_night = !g_weather_is_night;
            g_weather_shown = true;
            const auto& preset = kWeatherPresets[g_weather_preset_idx];
            widget_profile_card::set_default_weather(
                preset.cond, 72, widget_profile_card::TemperatureUnit::Fahrenheit, true,
                g_weather_is_night, preset.intensity);
            LOG("[scr] weather -> %s (condition=%d intensity=%d)\n",
                g_weather_is_night ? "night" : "day", (int)preset.cond, (int)preset.intensity);
            break;
        }
        case 'h':
            g_holiday_debug = !g_holiday_debug;
            holiday_data::set_debug_override(g_holiday_debug);
            LOG("[scr] holiday debug data -> %s (day 3 / day 20 of any month; press 'n' or 'v' again to see it)\n",
                g_holiday_debug ? "ON" : "off");
            break;
        case 'X':
            g_pc_connected = !g_pc_connected;
            LOG("[scr] PC link -> %s\n", g_pc_connected ? "connected" : "OFFLINE");
            state_machine::set_pc_connected(g_pc_connected);
            debug_apply_defaults();
            debug_load_meeting_default();
            break;
        case 'f': {
            lv_obj_t* base = screen_no_meetings::create();
            debug_load(base);
            modal_factory_reset::create(base);
            break;
        }
        case 'F':
            LOG("[scr] FACTORY RESET — wiping NVS + BLE bonds + rebooting\n");
            factory_reset::execute();
            break;
        case 'U': {
            lv_obj_t* base = screen_no_meetings::create();
            debug_load(base);
            modal_unpair_phone::create(base);
            break;
        }
        case 'I': {
            // Seed a mock incoming-call notification, then raise the banner over
            // the current screen so the layout/buttons can be reviewed without a
            // live ANCS call. Decline is a no-op here (no BLE link).
            app_state::set_ancs_detail(0xCA11u, "phone", "Phone",
                "Jane Appleseed", "", "mobile",
                /*recv_epoch=*/0, /*hhmm=*/"", "com.apple.mobilephone",
                app_state::AncsCategory::INCOMING_CALL, /*important=*/false, /*silent=*/false,
                /*pos_label=*/"Answer", /*neg_label=*/"Decline", /*neg_action=*/true);
            modal_incoming_call::show(0xCA11u);
            break;
        }
        case 'w': debug_load_setup(screen_setup::Step::Welcome,      false); break;
        case 'i': debug_load_setup(screen_setup::Step::Install,      false); break;
        case 'b': debug_load_setup(screen_setup::Step::Pairing,      false); break;
        case 'B': debug_load_setup(screen_setup::Step::Pairing,      true);  break;
        case 'o': { // Link Orion + orioning modal
                lv_obj_t* s = screen_setup::create(screen_setup::Step::Pairing);
            debug_load(s);
            screen_setup::show_orioning_modal(s);
            break;
        }
        case 't': debug_load_setup(screen_setup::Step::PhonePairing, false); break;
        case 'e': debug_load_setup(screen_setup::Step::Complete,     false); break;
        case 'x': debug_load(screen_reconnect_syncing::create());              break;
        // ── OTA update flow (button callbacks walk the flow for visual test) ──
        case 'u': debug_load(screen_ota_updating::create());                  break;
        case '1': {
            // "Installing… / screen goes dark" — set_installing() switches the
            // labels on the updating screen, so build that first, then switch.
            lv_obj_t* s = screen_ota_updating::create();
            screen_ota_updating::set_installing();
            debug_load(s);
            break;
        }
        case '2': debug_load(screen_ota_updating::create_updated_ack("1.0.1", dbg_ota_close)); break;
        case '3': debug_load(screen_ota_updating::create_error(
                      "The update couldn't be installed — try again from Orion",
                      dbg_ota_close)); break;
        case 'R':
            LOG("[scr] Re-running state_machine::evaluate()\n");
            apply_state_defaults();
            state_machine::evaluate();
            break;
        case 'M': print_mem_stats(); break;
        case '?': print_keymap(); break;
        default:
            if (c == '\r' || c == '\n' || c == ' ') return;
            LOG("[scr] unknown key '%c' (0x%02X)\n", c, (uint8_t)c);
            break;
    }
}

#endif // ORI_DEBUG_SERIAL

} // namespace

namespace screen_manager {

void init() {
    LOG("[scr] screen manager init\n");
    state_machine::init();
    apply_state_defaults();
    state_machine::evaluate();

#ifdef ORI_DEBUG_SERIAL
    print_keymap();
#endif
}

void poll_serial() {
#ifdef ORI_DEBUG_SERIAL
    // OTA owns the USB CDC port while a transfer is in flight or the frame
    // parser is mid-frame. Reading here would steal OTA bytes (ota_receiver::poll
    // returns mid-frame on its time budget). Stay off the port until it's idle.
    if (ota_receiver::is_active() || ota_receiver::is_busy()) return;

    while (Serial.available() > 0) {
        // Don't consume the first byte of a frame belonging to one of the two
        // binary protocols sharing this port: 0x4F starts an OTA frame
        // (ota_receiver), 0xA5 starts an Orinari IDENTIFY (identify_responder).
        const uint8_t next = (uint8_t)Serial.peek();
        if (next == 0x4F || next == 0xA5) break;
        int b = Serial.read();
        if (b > 0) debug_handle_key((char)b);
    }
#endif
}

} // namespace screen_manager
