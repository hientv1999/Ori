#include "state_machine.h"

#include <Arduino.h>
#include "ori_log.h"
#include <lvgl.h>
#include <time.h>
#include <string>
#include <set>
#include <vector>

#include "ble/ble_manager.h"
#include "factory_reset.h"
#include "app_state.h"
#include "nvs_store.h"
#include "ota_receiver.h"
#include "nvs_sync.h"
#include <cbor.h>
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

// Ori — State machine.
//
// Left-panel priority logic per state-machine.md. 1 s tick drives the
// 5-min countdown and meeting-list refresh. Two user-selectable modes:
// Calendar (0, default) and Media (1, requires PC link).
// Clock is a separate state entered by tapping the status-bar time;
// not part of the mode-toggle cycle; exits via the mode-toggle button.

namespace {

// ─── Constants ────────────────────────────────────────────────────────────

constexpr int ALERT_WINDOW_S    = 300; // 5 minutes in seconds
constexpr uint32_t TICK_MS      = 1000;

// ─── Module state ─────────────────────────────────────────────────────────

AppState g_state           = AppState::NO_MEETINGS; // current rendered state
uint8_t  g_mode            = 0;                     // 0=Calendar, 1=Media
uint8_t  g_pre_clock_mode  = 0;                     // mode to restore when leaving Clock
bool     g_pc_connected    = false; // true only once Orion's BLE link is confirmed
bool     g_phone_connected = false;
bool     g_phone_bonded    = false; // true when an iPhone bond exists in NVS
bool     g_force_rebuild   = false; // force evaluate() to rebuild even if state unchanged

// Most recent Teams presence pushed by Orion via the Presence Status
// characteristic (ble-protocol.md §3). 0x03 = Offline, matching the
// gatt_server default and the "never show stale presence" rule — never
// persisted to NVS (presence is ephemeral, §6.4).
uint8_t  g_presence_byte   = 0x03;

// Post-OTA acknowledgement (read from NVS at init). When pending, the boot
// screen is the "Firmware updated" ack and stays sticky until the user taps
// Close (which clears the NVS flag). g_ota_ack_version holds the new version.
bool     g_has_ota_ack     = false;
char     g_ota_ack_version[24] = {};

// Set of meeting start-times (encoded as minutes since midnight) for which
// the 5-minute alert has already fired this boot.  Cleared on reboot only.
std::set<int> g_alerted_meetings;

// The current LVGL screen object. Kept so we can destroy it before loading
// a new one, preventing memory leaks.
lv_obj_t* g_current_screen = nullptr;

// Countdown modal's parent screen (needed to overlay it on the right base).
lv_obj_t* g_countdown_base = nullptr;

// Reference to the periodic tick timer created in state_machine::init().
lv_timer_t* g_tick_timer = nullptr;

// ─── Runtime meeting cache ────────────────────────────────────────────────
// Populated from NVS blob at boot and updated on every BLE MeetingList write.
// Uses static char storage so app_state::Meeting const-char* fields are valid.

namespace {
struct RtMeeting {
    char     start_str[6];   // "HH:MM"
    char     end_str[6];
    char     title[129];
    char     loc[65];
    char     org[65];
    uint32_t start_epoch;
    uint32_t end_epoch;
};
RtMeeting         g_rt[32];
size_t            g_rt_count = 0;
app_state::Meeting g_rt_display[32];  // const-char* view into g_rt, rebuilt by set_meetings_cbor

static void epoch_to_hhmm(uint32_t epoch, char* buf, size_t sz) {
    time_t t = (time_t)epoch;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    snprintf(buf, sz, "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
}
} // namespace

// ─── Helpers ──────────────────────────────────────────────────────────────

bool is_pto_active() {
    uint32_t s = 0, e = 0;
    if (!nvs_sync::load_pto_meta(&s, &e, nullptr, 0)) return false;
    if (!s || !e) return false;
    uint32_t now = (uint32_t)time(nullptr);
    return now >= s && now <= e;
}

// Convert a "HH:MM" string from app_state to minutes-since-midnight.
static int hhmm_to_mins(const char* s) {
    if (!s || s[0] == '\0') return -1;
    int h = 0, m = 0;
    const char* p = s;
    while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
    if (*p == ':') ++p;
    while (*p >= '0' && *p <= '9') m = m * 10 + (*p++ - '0');
    return h * 60 + m;
}

// Current local time as minutes since midnight.
static int now_mins() {
    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    return tm_buf.tm_hour * 60 + tm_buf.tm_min;
}

// Current local time as seconds since midnight.
static long now_seconds() {
    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    return (long)tm_buf.tm_hour * 3600 + tm_buf.tm_min * 60 + tm_buf.tm_sec;
}

// Returns the live meeting list with past meetings removed and in_progress flag
// set. Static backing array — safe in single-threaded LVGL context; do not
// hold past the next call.
static app_state::MeetingList filtered_meetings() {
    static app_state::Meeting buf[32];
    app_state::MeetingList all = { g_rt_display, g_rt_count };
    int now_m = now_mins();
    uint32_t now_epoch = (uint32_t)time(nullptr);
    size_t n = 0;
    for (size_t i = 0; i < all.count && n < 32; ++i) {
        const app_state::Meeting& m = all.items[i];
        int end_m = hhmm_to_mins(m.end);
        if (end_m < 0 || end_m >= now_m) {
            buf[n] = m;
            // Recompute in_progress from epoch if available, else from string.
            if (g_rt_count > 0) {
                const RtMeeting& rt = g_rt[i];
                buf[n].in_progress = (now_epoch >= rt.start_epoch && now_epoch <= rt.end_epoch);
            }
            ++n;
        }
    }
    return { buf, n };
}

// Scan the mock meeting list and return true if we just entered a 5-min
// window for any un-alerted meeting. Populates out_* with the meeting data
// if returning true.
static bool check_countdown(const char** out_title,
                             const char** out_when,
                             int*         out_diff_s,
                             int*         out_key) {
    long now_s = now_seconds();
    app_state::MeetingList list = { g_rt_display, g_rt_count };

    for (size_t i = 0; i < list.count; ++i) {
        const app_state::Meeting& m = list.items[i];
        int start_mins = hhmm_to_mins(m.start);
        if (start_mins < 0) continue;
        long start_s = (long)start_mins * 60;

        long diff = start_s - now_s;
        if (diff < 0 || diff > ALERT_WINDOW_S) continue;

        // Within the 5-min window — check if already alerted this boot.
        if (g_alerted_meetings.count(start_mins)) continue;

        // New alert.
        *out_title = m.title;

        // Build "Starts at HH:MM" string — m.start is already "HH:MM".
        static char when_buf[32];
        snprintf(when_buf, sizeof(when_buf), "Starts at %s", m.start);
        *out_when = when_buf;

        *out_diff_s = (int)diff;
        *out_key = start_mins;
        return true;
    }
    return false;
}

// Apply the shared widget defaults (presence, PC link, mode) before any
// screen is created, so each screen picks up the current runtime state
// without requiring per-screen setup calls.
void apply_widget_defaults() {
    // Reflect the cached Presence Status value (pushed by Orion) while the PC
    // link is up; force Offline while it's down. Do NOT hardcode "Available"
    // here — this runs on every screen rebuild (mode toggle, meeting list
    // refresh, clock enter/exit, etc.) and a hardcoded value would clobber a
    // real Busy/Away presence back to Available on the next rebuild.
    widget_profile_card::set_default_presence(
        g_pc_connected
            ? static_cast<widget_profile_card::Presence>(g_presence_byte)
            : widget_profile_card::Presence::Offline);

    // Mode-toggle is shown when PC is connected OR when in Clock mode
    // (the toggle acts as a "return" button in Clock, works even offline).
    bool show_toggle = g_pc_connected || (g_state == AppState::CLOCK);
    widget_status_bar::set_default_pc_connected(show_toggle);
    widget_status_bar::set_default_phone_bonded(g_phone_bonded);
    widget_status_bar::set_default_phone_connected(g_phone_connected);

    // In Clock state the bar shows Mode::Clock so the toggle glyph reads
    // "return to previous mode"; otherwise reflect the current g_mode.
    widget_status_bar::Mode bar_mode;
    if (g_state == AppState::CLOCK) {
        bar_mode = widget_status_bar::Mode::Clock;
    } else {
        bar_mode = (g_mode == 1) ? widget_status_bar::Mode::Keyboard
                                 : widget_status_bar::Mode::Calendar;
    }
    widget_status_bar::set_default_mode(bar_mode);

    // Mode-toggle callback: advance mode or exit Clock.
    widget_status_bar::set_default_mode_toggle_cb([]() {
        state_machine::on_mode_toggle();
    });

    // Time-tap callback: enter Clock mode.
    widget_status_bar::set_default_time_tap_cb([]() {
        state_machine::on_clock_enter();
    });
}

// Transition to a new LVGL screen, deleting the old one.
// lv_refr_now() is intentionally absent — see debug_load() in screen_manager.cpp.
// auto_del=true: LVGL fires screen-unload events then deletes d->prev_scr before
// returning. Calling lv_obj_delete(prev) after lv_scr_load() instead leaves
// d->prev_scr dangling — if LVGL touches it on the next lv_timer_handler() pass
// (e.g. to fire LV_EVENT_SCREEN_UNLOADED) it reads freed memory and crashes.
void load_screen(lv_obj_t* new_screen) {
    g_current_screen = new_screen;
    lv_scr_load_anim(new_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, /*auto_del=*/true);
}

// ─── State-specific screen builders ───────────────────────────────────────

lv_obj_t* build_setup_screen() {
    // Resume at the furthest setup step reached before a power cycle. Latest
    // step wins: phone-pairing (synced) > Link-Orion (bonded, mid/awaiting sync)
    // > Welcome. The Link-Orion resume lets Orion reconnect via the stored bond
    // and re-drive the sync instead of forcing the user back to the start.
    if (nvs::is_awaiting_phone_pairing()) {
        return screen_setup::create(screen_setup::Step::PhonePairing);
    }
    if (nvs::is_awaiting_sync()) {
        return screen_setup::create(screen_setup::Step::Pairing);
    }
    return screen_setup::create(screen_setup::Step::Welcome);
}

lv_obj_t* build_ota_screen() {
    return screen_ota_updating::create();
}

// Close button on the post-update ack screen.
void ota_ack_close_cb(lv_event_t*) {
    state_machine::on_ota_ack_close();
}

lv_obj_t* build_ota_ack_screen() {
    return screen_ota_updating::create_updated_ack(g_ota_ack_version, ota_ack_close_cb);
}

lv_obj_t* build_pto_screen() {
    return screen_pto::create();
}

lv_obj_t* build_meeting_list_screen() {
    return screen_meeting_list::create(filtered_meetings(), false);
}

lv_obj_t* build_no_meetings_screen() {
    return screen_no_meetings::create();
}

lv_obj_t* build_clock_screen() {
    return screen_clock::create();
}

lv_obj_t* build_controls_screen() {
    return screen_media_mode::create();
}

lv_obj_t* build_reconnect_screen() {
    return screen_reconnect_syncing::create();
}

// Fire a countdown modal on top of the appropriate base screen.
// The base screen is created fresh (or reused if it's already the current one)
// and stored in g_countdown_base so we don't destroy it when the modal fires.
void fire_countdown(const char* title, const char* when, int diff_s) {
    // Build the base screen that sits under the modal.
    lv_obj_t* base = nullptr;
    if (g_mode == 1 && g_pc_connected) {
        base = build_controls_screen();
    } else {
        app_state::MeetingList list = filtered_meetings();
        if (list.count > 0) {
            base = build_meeting_list_screen();
        } else {
            base = build_no_meetings_screen();
        }
    }

    g_countdown_base = base;
    load_screen(base);
    modal_countdown::create(base, title, when, diff_s);
    g_state = AppState::COUNTDOWN;
}

// ─── Priority evaluator ───────────────────────────────────────────────────

AppState compute_target_state() {
    // OTA is the highest-priority, non-dismissable full-screen takeover. Pin it
    // to the live transfer flag, not g_state — otherwise any BLE event / tick
    // that resets g_state (set_pc_connected, etc.) yanks the screen away mid-
    // update and the firmware stops servicing the USB stream.
    if (ota_receiver::is_active())   return AppState::OTA_UPDATING;
    // Post-update ack survives reboots until acknowledged — show it before
    // anything else (a successful OTA implies the device is already provisioned).
    if (g_has_ota_ack)               return AppState::OTA_ACK;
    if (nvs::is_first_boot())        return AppState::SETUP;
    if (g_state == AppState::OTA_UPDATING) return AppState::OTA_UPDATING;
    if (g_state == AppState::RECONNECT_SYNCING) return AppState::RECONNECT_SYNCING;
    if (is_pto_active())             return AppState::PTO_ACTIVE;
    // Countdown is handled inline in tick() before evaluate() is called for
    // state changes, so if we reach here it has already been cleared.
    // Note: CLOCK is never returned here — it is entered exclusively via
    // on_clock_enter() (user taps the status-bar time) and protected in
    // evaluate() from being overwritten by normal Calendar state changes.

    app_state::MeetingList list = filtered_meetings();
    if (list.count > 0)              return AppState::MEETING_LIST;
    return AppState::NO_MEETINGS;
}

// ─── Periodic tick ────────────────────────────────────────────────────────

static void tick_cb(lv_timer_t* /*t*/) {
    // 5-minute pre-meeting alert.
    if (g_state != AppState::COUNTDOWN   &&
        g_state != AppState::SETUP       &&
        g_state != AppState::OTA_UPDATING &&
        !ota_receiver::is_active()       &&
        !is_pto_active()) {

        const char* title = nullptr;
        const char* when  = nullptr;
        int diff_s        = 0;
        int key           = -1;

        if (check_countdown(&title, &when, &diff_s, &key)) {
            g_alerted_meetings.insert(key);
            LOG("[sm] countdown alert for meeting key=%d\n", key);
            fire_countdown(title, when, diff_s);
            return;  // screen already updated; skip re-evaluate below
        }
    }

    state_machine::evaluate();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

namespace state_machine {

void init() {
    // Pre-load PTO metadata into RAM cache while the heap is clean (before any
    // screen is created). This prevents the NVS handle allocator from landing
    // on SRAM that was previously used by the media screen's LVGL allocations,
    // which would corrupt the NVSHandleSimple vtable pointer and crash.
    nvs_sync::prime_pto_cache();

    // Restore mode from NVS.
    g_mode = nvs::get_mode();

    // Check if an iPhone bond already exists in NVS (survives reboots).
    {
        uint8_t iphone_addr[6] = {};
        ble_manager::load_bond_addr(ble_manager::BOND_KEY_IPHONE, iphone_addr);
        g_phone_bonded = !ble_manager::is_bond_slot_empty(iphone_addr);
    }

    // Pending post-OTA acknowledgement? If so the first screen this boot is the
    // "Firmware updated" ack (sticky until the user taps Close).
    g_has_ota_ack = nvs::get_ota_ack(g_ota_ack_version, sizeof(g_ota_ack_version));
    if (g_has_ota_ack)
        LOG("[sm] post-OTA ack pending (version %s)\n", g_ota_ack_version);

    // Create the periodic evaluation timer (1 s cadence).
    g_tick_timer = lv_timer_create(tick_cb, TICK_MS, nullptr);

    LOG("[sm] init: mode=%d first_boot=%d\n",
                  (int)g_mode, (int)nvs::is_first_boot());
}

AppState evaluate() {
    AppState target = compute_target_state();

    // COUNTDOWN: user must tap Close; not overridden by normal state changes.
    if (g_state == AppState::COUNTDOWN && target != AppState::SETUP) {
        return g_state;
    }

    // CLOCK: user-entered by tapping the time; persists through normal
    // Calendar state changes (meeting list updates, etc.). Only overridden
    // by high-priority states or an explicit on_mode_toggle() call.
    bool force = g_force_rebuild;
    g_force_rebuild = false;
    if (!force && g_state == AppState::CLOCK &&
        target != AppState::SETUP &&
        target != AppState::OTA_UPDATING &&
        target != AppState::PTO_ACTIVE) {
        return g_state;
    }

    if (!force && target == g_state && g_current_screen != nullptr) {
        return g_state;   // no change; avoid needless rebuilds
    }

    g_state = target;
    apply_widget_defaults();

    lv_obj_t* new_screen = nullptr;

    switch (g_state) {
        case AppState::SETUP:
            new_screen = build_setup_screen();
            break;

        case AppState::OTA_UPDATING:
            new_screen = build_ota_screen();
            break;

        case AppState::OTA_ACK:
            new_screen = build_ota_ack_screen();
            break;

        case AppState::PTO_ACTIVE:
            new_screen = build_pto_screen();
            break;

        case AppState::COUNTDOWN:
            // Countdown is raised inside tick_cb with fire_countdown().
            // If evaluate() is called while COUNTDOWN is the target, it means
            // we missed re-entering via tick — just stay on whatever is loaded.
            return g_state;

        case AppState::RECONNECT_SYNCING:
            new_screen = build_reconnect_screen();
            break;

        case AppState::MEETING_LIST:
            if (g_mode == 1 && g_pc_connected) {
                new_screen = build_controls_screen();
            } else {
                new_screen = build_meeting_list_screen();
            }
            break;

        case AppState::NO_MEETINGS:
            if (g_mode == 1 && g_pc_connected) {
                new_screen = build_controls_screen();
            } else {
                new_screen = build_no_meetings_screen();
            }
            break;

        case AppState::CLOCK:
            new_screen = build_clock_screen();
            break;
    }

    if (new_screen) {
        load_screen(new_screen);
        LOG("[sm] state -> %d  mode=%d\n", (int)g_state, (int)g_mode);
    }

    return g_state;
}

// Deferred work flags — set from LVGL timer callbacks, drained in poll()
// which runs in the main loop before lv_timer_handler(). NVS flash writes
// disable ICache/DCache system-wide; doing them inside an LVGL callback
// while LCD_CAM DMA is active on Core 1 triggers the interrupt watchdog.
static bool g_setup_complete_pending = false;
static bool g_unpair_phone_pending   = false;
static bool g_phone_wipe_pending     = false;  // stale-bond wipe before re-pair
static bool g_mode_write_pending     = false;  // persist g_mode (set from on_mode_toggle)

void set_meetings_cbor(const uint8_t* buf, size_t len, bool save_to_nvs) {
    if (!buf || !len) return;

    if (save_to_nvs) nvs_sync::save_meetings_blob(buf, len);

    // Parse CBOR: { "date": uint, "items": [ { "start": uint, "end": uint,
    //   "title": text, "loc": text, "org": text }, ... ] }
    CborParser parser;
    CborValue  root;
    if (cbor_parser_init(buf, len, 0, &parser, &root) != CborNoError) return;
    if (!cbor_value_is_map(&root)) return;

    CborValue map;
    cbor_value_enter_container(&root, &map);

    CborValue items_val;
    bool found_items = false;

    while (!cbor_value_at_end(&map)) {
        char key[16] = {};
        size_t key_len = sizeof(key) - 1;
        if (!cbor_value_is_text_string(&map)) { cbor_value_advance(&map); continue; }
        cbor_value_copy_text_string(&map, key, &key_len, &map);
        if (cbor_value_at_end(&map)) break;

        if (strcmp(key, "items") == 0 && cbor_value_is_array(&map)) {
            items_val = map;
            found_items = true;
        }
        cbor_value_advance(&map);
    }

    if (!found_items) return;

    CborValue item;
    cbor_value_enter_container(&items_val, &item);
    size_t count = 0;

    while (!cbor_value_at_end(&item) && count < 32) {
        if (!cbor_value_is_map(&item)) { cbor_value_advance(&item); continue; }

        RtMeeting& rt = g_rt[count];
        memset(&rt, 0, sizeof(rt));

        CborValue field;
        cbor_value_enter_container(&item, &field);
        while (!cbor_value_at_end(&field)) {
            char fkey[16] = {};
            size_t fkey_len = sizeof(fkey) - 1;
            if (!cbor_value_is_text_string(&field)) { cbor_value_advance(&field); continue; }
            cbor_value_copy_text_string(&field, fkey, &fkey_len, &field);
            if (cbor_value_at_end(&field)) break;

            if (strcmp(fkey, "start") == 0 && cbor_value_is_unsigned_integer(&field)) {
                uint64_t v; cbor_value_get_uint64(&field, &v);
                rt.start_epoch = (uint32_t)v;
                epoch_to_hhmm(rt.start_epoch, rt.start_str, sizeof(rt.start_str));
            } else if (strcmp(fkey, "end") == 0 && cbor_value_is_unsigned_integer(&field)) {
                uint64_t v; cbor_value_get_uint64(&field, &v);
                rt.end_epoch = (uint32_t)v;
                epoch_to_hhmm(rt.end_epoch, rt.end_str, sizeof(rt.end_str));
            } else if (strcmp(fkey, "title") == 0 && cbor_value_is_text_string(&field)) {
                size_t sz = sizeof(rt.title) - 1;
                cbor_value_copy_text_string(&field, rt.title, &sz, nullptr);
            } else if (strcmp(fkey, "loc") == 0 && cbor_value_is_text_string(&field)) {
                size_t sz = sizeof(rt.loc) - 1;
                cbor_value_copy_text_string(&field, rt.loc, &sz, nullptr);
            } else if (strcmp(fkey, "org") == 0 && cbor_value_is_text_string(&field)) {
                size_t sz = sizeof(rt.org) - 1;
                cbor_value_copy_text_string(&field, rt.org, &sz, nullptr);
            }
            if (!cbor_value_at_end(&field)) cbor_value_advance(&field);
        }
        cbor_value_leave_container(&item, &field);

        // Build the app_state::Meeting view.
        g_rt_display[count] = {
            rt.start_str, rt.end_str,
            rt.title, rt.loc, rt.org,
            false,  // overlap — computed below
            false   // in_progress — computed in filtered_meetings()
        };
        ++count;
        cbor_value_advance(&item);
    }

    g_rt_count = count;

    // Compute overlap: two meetings overlap when one starts before the other ends.
    for (size_t i = 0; i < g_rt_count; ++i) {
        for (size_t j = i + 1; j < g_rt_count; ++j) {
            if (g_rt[i].start_epoch < g_rt[j].end_epoch &&
                g_rt[j].start_epoch < g_rt[i].end_epoch) {
                g_rt_display[i].overlap = true;
                g_rt_display[j].overlap = true;
            }
        }
    }

    LOG("[sm] meetings parsed: %u items\n", (unsigned)g_rt_count);
}

void on_setup_complete() {
    LOG("[sm] on_setup_complete — deferring NVS write to poll()\n");
    g_setup_complete_pending = true;
}

void poll() {
    if (g_setup_complete_pending) {
        g_setup_complete_pending = false;
        LOG("[sm] poll: setup complete — writing NVS + evaluating\n");
        nvs::mark_setup_complete();
        evaluate();
    }
    if (g_unpair_phone_pending) {
        g_unpair_phone_pending = false;
        LOG("[sm] poll: unpair phone — wiping bond + evaluating\n");
        ble_manager::wipe_iphone_bond();
        ble_manager::restart_advertising();
        g_force_rebuild = true;
        g_state = AppState::NO_MEETINGS;  // break out of any early-return guard
        evaluate();
    }
    if (g_phone_wipe_pending) {
        g_phone_wipe_pending = false;
        LOG("[sm] poll: wiping stale iPhone bond for re-pair\n");
        ble_manager::wipe_iphone_bond();
        // No evaluate() — the re-pair screen the user just opened owns the
        // display. Restart advertising so the now-empty iPhone slot plus the
        // open pairing window put the ANCS solicitation on air.
        ble_manager::restart_advertising();
    }
    if (g_mode_write_pending) {
        g_mode_write_pending = false;
        nvs::set_mode(g_mode);  // persist final mode off the LVGL timer stack
    }
}

void on_factory_reset() {
    LOG("[sm] on_factory_reset\n");
    // Delegate to the shared factory_reset::execute() so both the local
    // long-press path and the remote BLE path converge here.
    factory_reset::execute();
}

void on_unpair_phone() {
    // Called from the unpair modal's LVGL button callback. Only RAM/UI flags
    // are touched here; the bond wipe (NimBLE delete + NVS flash write) and
    // screen rebuild are deferred to poll() — running them on the event-
    // dispatch stack made the nvs_set_blob zeroing the iphone_addr slot fail
    // (Preferences "OTHER"), leaving a ghost bond that blocked re-pairing.
    LOG("[sm] on_unpair_phone — deferring bond wipe to poll()\n");
    g_phone_bonded = false;
    widget_status_bar::set_all_phone_bonded(false);
    g_unpair_phone_pending = true;
}

void request_phone_bond_wipe() {
    // Tapping the phone icon while the iPhone is bonded but disconnected
    // opens the re-pair screen; the stale bond must go or the iPhone slot
    // stays full and the ANCS solicitation never advertises. Same poll()
    // deferral as on_unpair_phone (NVS write off the LVGL stack), but no
    // screen rebuild — the re-pair screen is about to load.
    LOG("[sm] request_phone_bond_wipe — deferring to poll()\n");
    g_phone_bonded = false;
    widget_status_bar::set_all_phone_bonded(false);
    g_phone_wipe_pending = true;
}

void on_mode_toggle() {
    if (g_state == AppState::CLOCK) {
        // In Clock: return to the mode that was active before the time tap.
        g_mode = g_pre_clock_mode;
        LOG("[sm] clock exit -> mode=%d\n", (int)g_mode);
        g_force_rebuild = true;
        g_state = AppState::NO_MEETINGS;  // break Clock protection in evaluate()
        evaluate();
        return;
    }

    // Normal 2-mode cycle: Calendar (0) ↔ Media (1).
    g_mode = (g_mode == 0) ? 1 : 0;

    // If switching to Media but PC is offline, revert immediately.
    if (g_mode == 1 && !g_pc_connected) {
        g_mode = 0;
        LOG("[sm] PC offline — Media mode reverted to Calendar\n");
    }
    LOG("[sm] mode toggle -> %s\n", g_mode ? "Media" : "Calendar");

    // Persist the final mode in poll(), NOT here. on_mode_toggle runs inside
    // an LVGL timer callback (the deferred-toggle 1 ms timer); an NVS flash
    // write in that context collides with LCD_CAM DMA / cache-disable and
    // crashes deep in the NVS page walker (LoadProhibited). poll() drains the
    // write on a clean main-loop stack — same rule as every other NVS write.
    g_mode_write_pending = true;

    g_force_rebuild = true;
    g_state = AppState::NO_MEETINGS;  // ensure evaluate() rebuilds
    evaluate();
}

void on_clock_enter() {
    if (g_state == AppState::CLOCK) return;  // already in Clock, no-op
    g_pre_clock_mode = g_mode;
    g_state = AppState::CLOCK;
    apply_widget_defaults();
    load_screen(build_clock_screen());
    LOG("[sm] clock enter (pre_mode=%d)\n", (int)g_pre_clock_mode);
}

void on_ota_begin() {
    LOG("[sm] on_ota_begin\n");
    // Pause the 1 s meeting-check tick for the whole update: no meeting-expiry,
    // 5-minute-alert, or evaluate() work should run (or touch state) while the
    // OTA owns the device. Resumed in on_reconnect_end() if the update fails.
    if (g_tick_timer) lv_timer_pause(g_tick_timer);
    g_state = AppState::OTA_UPDATING;
    apply_widget_defaults();
    load_screen(build_ota_screen());
}

void ota_show(lv_obj_t* screen) {
    // ota_receiver built a flow screen (Update failed). Keep the meeting-check
    // tick paused (a BEGIN-reject error screen can reach here without on_ota_begin).
    if (g_tick_timer) lv_timer_pause(g_tick_timer);
    g_state = AppState::OTA_UPDATING;   // is_active() keeps it sticky
    apply_widget_defaults();
    load_screen(screen);
}

void on_ota_ack_close() {
    LOG("[sm] post-OTA ack acknowledged\n");
    nvs::clear_ota_ack();
    g_has_ota_ack = false;
    g_force_rebuild = true;
    g_state = AppState::NO_MEETINGS;
    evaluate();
}

void on_reconnect_begin() {
    // Never overlay the reconnect-syncing UI on top of an OTA takeover.
    if (ota_receiver::is_active()) return;
    LOG("[sm] on_reconnect_begin\n");
    g_state = AppState::RECONNECT_SYNCING;
    apply_widget_defaults();
    load_screen(build_reconnect_screen());
}

void on_reconnect_end() {
    LOG("[sm] on_reconnect_end\n");
    // Resume the meeting-check tick if an OTA had paused it (no-op otherwise —
    // this is also the normal BLE reconnect-overlay exit). dismiss_error() routes
    // a failed/aborted OTA here.
    if (g_tick_timer) lv_timer_resume(g_tick_timer);
    // Clear the RECONNECT_SYNCING sentinel so compute_target_state() runs
    // the full logic (it early-returns RECONNECT_SYNCING when g_state matches).
    g_force_rebuild = true;
    g_state = AppState::NO_MEETINGS;
    evaluate();
}

void set_presence(uint8_t presence_byte) {
    g_presence_byte = presence_byte;
    widget_profile_card::set_default_presence(
        g_pc_connected
            ? static_cast<widget_profile_card::Presence>(g_presence_byte)
            : widget_profile_card::Presence::Offline);
}

void set_pc_connected(bool connected) {
    bool changed = (connected != g_pc_connected);
    g_pc_connected = connected;

    if (!connected) {
        // Never show a stale presence after the link drops — reset so a
        // future reconnect starts from Offline until Orion re-pushes.
        g_presence_byte = 0x03;
        if (g_mode == 1) {
            g_mode = 0;
            nvs::set_mode(0);
            LOG("[sm] PC disconnected — Media mode reverted to Calendar\n");
        }
        // If user was in Clock, ensure the return-to mode is also Calendar.
        if (g_pre_clock_mode == 1) {
            g_pre_clock_mode = 0;
        }
    }

    // Track the flag, but never rebuild the screen during an OTA takeover — a
    // BLE connect/disconnect mid-update must not yank the OTA screen or starve
    // the USB CDC poll. evaluate() runs (and resyncs) once the transfer ends.
    if (changed && !ota_receiver::is_active()) {
        g_force_rebuild = true;
        g_state = AppState::NO_MEETINGS;  // ensure evaluate() does a full rebuild
        evaluate();
    }
}

void set_phone_connected(bool connected) {
    g_phone_connected = connected;
    // Drive the status-bar phone icon directly from the authoritative BLE link
    // signal (this function is called from ble_manager on every iPhone
    // connect/disconnect). Do NOT rely on the ANCS notification queue for this:
    // it only reports "connected" once the first notification arrives, which
    // left the icon red — and the tap on the disconnected path — during the
    // window between connect and the first notification.
    widget_status_bar::set_all_phone_connected(connected);
    widget_status_bar::set_default_phone_connected(connected);
    if (connected) {
        // Connecting implies a bond now exists — persist the flag and update the
        // active bar immediately so tapping the icon goes to the unpair modal
        // rather than the pairing screen.
        g_phone_bonded = true;
        widget_status_bar::set_all_phone_bonded(true);
    }
    // On disconnect don't clear g_phone_bonded — the bond still exists in NVS.
    // The flag is cleared only when the user explicitly unpairs (on_unpair_phone).
}

AppState current_state() {
    return g_state;
}

uint8_t current_mode() {
    return g_mode;
}

} // namespace state_machine
