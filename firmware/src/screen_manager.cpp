#include "screen_manager.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "mock_data.h"
#include "nvs_store.h"
#include "state_machine.h"
#include "screens/modal_countdown.h"
#include "screens/modal_factory_reset.h"
#include "screens/modal_unpair_phone.h"
#include "screens/screen_clock.h"
#include "screens/screen_media_mode.h"
#include "screens/screen_meeting_list.h"
#include "screens/screen_no_meetings.h"
#include "screens/screen_ota_updating.h"
#include "screens/screen_pto.h"
#include "screens/screen_reconnect_syncing.h"
// #include "screens/screen_repair_phone.h" // removed obsolete repair screen
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

widget_profile_card::Presence g_presence    = widget_profile_card::Presence::Offline;
bool                          g_pc_connected = true;
widget_status_bar::Mode       g_status_mode  = widget_status_bar::Mode::Calendar;

// ─────────────────────────────────────────────────────────────────────────────
// apply_state_defaults — set widget defaults to current runtime values before
// any screen create() call.  Called by the debug cycler before each load and
// by state_machine::evaluate() internally.
// ─────────────────────────────────────────────────────────────────────────────

void apply_state_defaults() {
    auto eff = g_pc_connected ? g_presence : widget_profile_card::Presence::Offline;
    widget_profile_card::set_default_presence(eff);
    widget_status_bar::set_default_pc_connected(g_pc_connected);
    widget_status_bar::set_default_mode(g_status_mode);
    widget_status_bar::set_default_mode_toggle_cb([]() {
        state_machine::on_mode_toggle();
    });
}

// ORI_DEBUG_SERIAL serial cycler — gated by ORI_DEBUG_SERIAL in platformio.ini.
// Each key loads a screen directly, bypassing the state machine.

#ifdef ORI_DEBUG_SERIAL

const mock_data::AncsConfig k_default_ancs = {
    {
        "gmail", "slack", "whatsapp", "facetime", "messenger",
        "instagram", "discord", "teams", "reddit", "uber",
        "spotify", "youtube", "telegram", "amazon", "tiktok",
    },
    15, true,
};
const mock_data::AncsConfig k_no_phone = {
    { nullptr, nullptr, nullptr, nullptr, nullptr }, 0, false,
};

lv_obj_t* g_debug_screen = nullptr;

void debug_load(lv_obj_t* scr) {
    lv_obj_t* prev = g_debug_screen;
    g_debug_screen = scr;
    lv_scr_load(scr);
    lv_refr_now(lv_display_get_default());
    if (prev && prev != scr) lv_obj_delete(prev);
}

void debug_load_meeting_default() {
    mock_data::set_ancs_config(k_default_ancs);
    debug_load(screen_meeting_list::create(mock_data::meetings(), false));
}

void debug_load_setup(screen_setup::Step st, bool with_passkey) {
    mock_data::set_ancs_config(k_default_ancs);
    lv_obj_t* s = screen_setup::create(st);
    debug_load(s);
    if (with_passkey) screen_setup::show_passkey_modal(s);
}

void debug_apply_defaults() {
    auto eff = g_pc_connected ? g_presence : widget_profile_card::Presence::Offline;
    widget_profile_card::set_default_presence(eff);
    widget_status_bar::set_default_pc_connected(g_pc_connected);
    widget_status_bar::set_default_mode(g_status_mode);
    widget_status_bar::set_default_mode_toggle_cb([]() {
        g_status_mode = (g_status_mode == widget_status_bar::Mode::Calendar)
                      ? widget_status_bar::Mode::Keyboard
                      : widget_status_bar::Mode::Calendar;
        debug_apply_defaults();
        if (g_status_mode == widget_status_bar::Mode::Keyboard) {
            mock_data::set_ancs_config(k_default_ancs);
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
    Serial.println();
    Serial.println("=== Memory stats ===");
    Serial.printf("  SRAM   free %6u KB  used %6u KB  total %6u KB\n",
                  sram_free/1024, (sram_total-sram_free)/1024, sram_total/1024);
    Serial.printf("         largest block %u KB   min free since boot %u KB\n",
                  sram_lfb/1024, sram_min/1024);
    Serial.printf("  PSRAM  free %6u KB  used %6u KB  total %6u KB\n",
                  psram_free/1024, (psram_total-psram_free)/1024, psram_total/1024);
    Serial.printf("         largest block %u KB   min free since boot %u KB\n",
                  psram_lfb/1024, psram_min/1024);
    Serial.println("====================");
    Serial.println();
}

void print_keymap() {
    Serial.println();
    Serial.println("=== Ori screen cycler (ORI_DEBUG_SERIAL) ===");
    Serial.println("  m   Meeting list");
    Serial.println("  n   No meetings today");
    Serial.println("  c   Digital clock (entered via time tap)");
    Serial.println("  p   PTO scenic");
    Serial.println("  k   Media mode");
    Serial.println("  C   5-minute countdown modal");
    Serial.println("  P   Cycle Teams presence");
    Serial.println("  X   Toggle PC link state");
    Serial.println("  f   Factory reset modal");
    Serial.println("  U   Unpair iPhone modal");
    Serial.println("  w   Setup — Welcome");
    Serial.println("  i   Setup — Step 1 Install Orion");
    Serial.println("  b   Setup — Step 2 Link Orion");
    Serial.println("  B   Setup — Step 2 Passkey Modal");
    Serial.println("  o   Setup — Step 2 Orioning Modal");
    Serial.println("  t   Setup — Step 3 iPhone pairing");
    Serial.println("  e   Setup — Complete");
    Serial.println("  x   Reconnect-Syncing overlay");
    Serial.println("  u   OTA-Updating");
    Serial.println("  R   Re-run state machine evaluate() (real boot logic)");
    Serial.println("  M   Print SRAM / PSRAM free stats");
    Serial.println("  ?   Print this map");
    Serial.println("===============================================");
    Serial.println();
}

void debug_handle_key(char c) {
    debug_apply_defaults();
    switch (c) {
        case 'm':
            mock_data::set_ancs_config(k_default_ancs);
            debug_load(screen_meeting_list::create(mock_data::meetings(), false));
            break;
        case 'n':
            mock_data::set_ancs_config(k_default_ancs);
            debug_load(screen_no_meetings::create());
            break;
        case 'c':
            mock_data::set_ancs_config(k_default_ancs);
            debug_load(screen_clock::create());
            break;
        case 'p':
            mock_data::set_ancs_config(k_default_ancs);
            debug_load(screen_pto::create());
            break;
        case 'C': {
            mock_data::set_ancs_config(k_default_ancs);
            lv_obj_t* base = screen_meeting_list::create(mock_data::meetings(), false);
            debug_load(base);
            modal_countdown::create(base, "Industrial design review",
                "Starts at 10:30 \xc2\xb7 Studio", 187);
            break;
        }
        case 'k': {
            mock_data::set_ancs_config(k_default_ancs);
            g_status_mode = widget_status_bar::Mode::Keyboard;
            debug_apply_defaults();
            debug_load(screen_media_mode::create());
            break;
        }
        case 'P': {
            using P = widget_profile_card::Presence;
            switch (g_presence) {
                case P::Available: g_presence = P::Busy;      break;
                case P::Busy:      g_presence = P::Away;      break;
                case P::Away:      g_presence = P::Offline;   break;
                default:           g_presence = P::Available; break;
            }
            debug_apply_defaults();
            debug_load_meeting_default();
            break;
        }
        case 'X':
            g_pc_connected = !g_pc_connected;
            Serial.printf("[scr] PC link -> %s\n", g_pc_connected ? "connected" : "OFFLINE");
            state_machine::set_pc_connected(g_pc_connected);
            debug_apply_defaults();
            debug_load_meeting_default();
            break;
        case 'f': {
            mock_data::set_ancs_config(k_default_ancs);
            lv_obj_t* base = screen_meeting_list::create(mock_data::meetings(), false);
            debug_load(base);
            modal_factory_reset::create(base);
            break;
        }
        case 'U': {
            mock_data::set_ancs_config(k_default_ancs);
            lv_obj_t* base = screen_meeting_list::create(mock_data::meetings(), false);
            debug_load(base);
            modal_unpair_phone::create(base);
            break;
        }
        case 'w': debug_load_setup(screen_setup::Step::Welcome,      false); break;
        case 'i': debug_load_setup(screen_setup::Step::Install,      false); break;
        case 'b': debug_load_setup(screen_setup::Step::Pairing,      false); break;
        case 'B': debug_load_setup(screen_setup::Step::Pairing,      true);  break;
        case 'o': { // Link Orion + orioning modal
            mock_data::set_ancs_config(k_default_ancs);
            lv_obj_t* s = screen_setup::create(screen_setup::Step::Pairing);
            debug_load(s);
            screen_setup::show_orioning_modal(s);
            break;
        }
        case 't': debug_load_setup(screen_setup::Step::PhonePairing, false); break;
        case 'e': debug_load_setup(screen_setup::Step::Complete,     false); break;
        // case 'r': debug_load(screen_repair_phone::create()); // removed obsolete repair screen
        case 'x': mock_data::set_ancs_config(k_default_ancs);
                  debug_load(screen_reconnect_syncing::create());              break;
        case 'u': debug_load(screen_ota_updating::create());                  break;
        case 'R':
            Serial.println("[scr] Re-running state_machine::evaluate()");
            apply_state_defaults();
            state_machine::evaluate();
            break;
        case 'M': print_mem_stats(); break;
        case '?': print_keymap(); break;
        default:
            if (c == '\r' || c == '\n' || c == ' ') return;
            Serial.printf("[scr] unknown key '%c' (0x%02X)\n", c, (uint8_t)c);
            break;
    }
}

#endif // ORI_DEBUG_SERIAL

} // namespace

namespace screen_manager {

void init() {
    Serial.println("[scr] screen manager init");
    state_machine::init();
    apply_state_defaults();
    state_machine::evaluate();

#ifdef ORI_DEBUG_SERIAL
    g_debug_screen = lv_screen_active();
    print_keymap();
#endif
}

void poll_serial() {
#ifdef ORI_DEBUG_SERIAL
    while (Serial.available() > 0) {
        int b = Serial.read();
        if (b > 0) debug_handle_key((char)b);
    }
#endif
}

} // namespace screen_manager
