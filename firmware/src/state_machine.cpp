#include "state_machine.h"

#include <Arduino.h>
#include "ori_log.h"
#include <lvgl.h>
#include <time.h>

#include "ble/ble_manager.h"
#include "ble/ancs_client.h"
#include "factory_reset.h"
#include "app_state.h"
#include "nvs_store.h"
#include "time_format.h"
#include "ota_receiver.h"
#include "nvs_sync.h"
#include <cbor.h>
#include "screens/modal_countdown.h"
#include "screens/modal_device_alert.h"
#include "screens/modal_iphone_info.h"
#include "screens/screen_calendar.h"
#include "screens/screen_clock.h"
#include "screens/screen_clock_analog.h"
#include "screens/screen_media_mode.h"
#include "screens/screen_meeting_list.h"
#include "screens/screen_no_meetings.h"
#include "screens/screen_ota_updating.h"
#include "screens/screen_time_off.h"
#include "screens/screen_reconnect_syncing.h"
#include "screens/screen_setup.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Ori — State machine.
//
// Left-panel priority logic per state-machine.md. 1 s tick drives the
// 5-min countdown and meeting-list refresh. Two user-selectable modes:
// Calendar (0, default) and Media (1, requires PC link).
// Clock and Calendar (month view) are separate states entered by tapping /
// long-pressing the status-bar time respectively; neither is part of the
// mode-toggle cycle; both exit via the mode-toggle button.

namespace {

// ─── Constants ────────────────────────────────────────────────────────────

constexpr int ALERT_WINDOW_S      = 300;  // 5 minutes in seconds
constexpr uint32_t TICK_MS        = 1000;
constexpr uint32_t RECONNECT_MIN_MS = 300; // minimum overlay visibility
constexpr uint32_t ALERT_CHECK_TICKS = 60; // Weather/Low-Battery Alert cadence
                                            // (~60s, matching Orion's own
                                            // reminders.rs/checkLowBattery()
                                            // poll interval — pc-app.md)

// ─── Module state ─────────────────────────────────────────────────────────

AppState g_state           = AppState::NO_MEETINGS; // current rendered state
uint8_t  g_mode            = 0;                     // 0=Calendar, 1=Media
uint8_t  g_pre_clock_mode  = 0;                     // mode to restore when leaving Clock/Calendar
uint8_t  g_clock_face      = 0;                     // 0=Digital, 1=Analog (nvs::get/set_clock_face)
bool     g_pc_connected    = false; // true only once Orion's BLE link is confirmed
bool     g_pc_synced       = false; // true only once a full sync has landed — see set_pc_synced()
bool     g_phone_connected = false;
bool     g_phone_bonded    = false; // true when an iPhone bond exists in NVS
bool     g_force_rebuild   = false; // force evaluate() to rebuild even if state unchanged

// True while the Setup-Complete screen is showing its checkmark + 5 s linger.
// build_complete() calls nvs::mark_setup_complete() up front (for power-cycle
// safety), which flips is_first_boot() to false — so compute_target_state()
// would immediately resolve to a runtime screen and the next tick/evaluate()
// would yank the Complete screen away mid-animation. This holds the screen put
// until the 5 s timer fires on_setup_complete(), which clears the flag.
bool     g_setup_complete_hold = false;

// Most recent weather condition + temperature + unit + day/night + intensity
// pushed by Orion via the Device Settings "w"/"d"/"u"/"n"/"i" fields
// (ble-protocol.md §3/§4). There is no
// "unverified" enum value to fall back to, so g_weather_valid
// gates visibility instead — apply_widget_defaults() hides the icon/text
// entirely (rather than rendering a fallback condition/temp/unit) whenever
// the PC link is down or Orion has never sent weather this boot. The cached
// condition/temp/unit/is_night/intensity are NOT reset on disconnect — only
// g_weather_valid is — there's no reason to discard the last-known values,
// just stop showing them until Orion re-confirms.
uint8_t  g_weather_condition_byte = 0;
int16_t  g_weather_temp_f         = 0;
uint8_t  g_weather_unit_byte      = 0;  // 0=Fahrenheit 1=Celsius
bool     g_weather_is_night       = false;
uint8_t  g_weather_intensity      = 0;  // widget_profile_card::WeatherIntensity, 0=None
bool     g_weather_valid          = false;

// Weather Alert / Low Battery Alert latch state (state-machine.md's "Weather
// Alert / Low Battery Alert overlays" section) — plain RAM globals, not NVS
// (both reset on reboot, matching Orion's own in-memory latch state).
// g_weather_alert_fired_today: cleared on day rollover (check_day_rollover()
// below, alongside g_alerted_mins) so the alert can fire again the next
// qualifying day. g_low_battery_alert_latched: cleared on recovery above
// low_battery_threshold_pct (check_device_alerts() below) OR the phone
// reconnecting (set_phone_connected()) — mirrors pc-app.md's
// checkLowBattery() semantics exactly.
bool     g_weather_alert_fired_today = false;
bool     g_low_battery_alert_latched = false;

// Post-OTA acknowledgement (read from NVS at init). When pending, the boot
// screen is the "Firmware updated" ack and stays sticky until the user taps
// Close (which clears the NVS flag). g_ota_ack_version holds the new version.
bool     g_has_ota_ack     = false;
char     g_ota_ack_version[24] = {};

// Timestamp (millis()) when the reconnect overlay was last shown.
// Used by on_reconnect_end() to guarantee RECONNECT_MIN_MS visibility.
uint32_t g_reconnect_shown_ms = 0;

// Whether the 5-minute alert has already fired this boot for the meeting
// starting at a given minute-of-day. Cleared on reboot only. Minute-of-day is
// naturally bounded to [0, 1439) by localtime_r()'s tm_hour/tm_min ranges, so
// a fixed bitmap (zero heap, O(1) lookup) replaces what used to be a
// std::set<int> — a red-black tree node-allocated on every unique insert,
// pure overhead for a key space this small and already bounded.
constexpr int MINUTES_PER_DAY = 1440;
bool g_alerted_mins[MINUTES_PER_DAY] = {};

// The current LVGL screen object. Kept so we can destroy it before loading
// a new one, preventing memory leaks.
lv_obj_t* g_current_screen = nullptr;

// The live calendar-runtime screen (NO_MEETINGS / MEETING_LIST) and its inner
// containers, registered by those screens' create() via
// register_runtime_calendar(). Non-null ONLY while such a screen is on top —
// cleared on its LV_EVENT_DELETE. This is what lets a reconnect sync refresh
// just the left panel in place (status bar + profile card untouched, no
// full-screen teardown/reflow) instead of rebuilding the whole screen.
lv_obj_t* g_runtime_screen = nullptr;
lv_obj_t* g_runtime_body   = nullptr;
lv_obj_t* g_runtime_left   = nullptr;

// True while a reconnect sync is showing the in-place left-panel overlay
// (as opposed to the full-screen fallback used when there's no live calendar
// screen to overlay). Decides which teardown path on_reconnect_end() takes.
bool g_reconnect_overlay_mode = false;

// Set by mark_display_needs_repaint() (called from gatt_server::
// run_staged_commit() whenever this sync's NVS write blanked the
// framebuffer) and consumed exactly once by finish_reconnect_now() — see
// mark_display_needs_repaint()'s own doc comment in state_machine.h for why
// this can't just reuse g_reconnect_overlay_mode.
bool g_needs_repaint = false;

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

// Drop glyphs the UI font can't render (emoji, CJK, …) from a meeting field,
// in place. sanitize_text needs distinct in/out, so route through a temp.
static void sanitize_inplace(char* buf, size_t sz) {
    char tmp[129];                 // >= largest meeting field (title)
    if (sz == 0 || sz > sizeof(tmp)) return;
    ui::sanitize_text(buf, tmp, sz);
    memcpy(buf, tmp, sz);
}
} // namespace

// ─── Helpers ──────────────────────────────────────────────────────────────

bool is_time_off_active() {
    uint32_t s = 0, e = 0;
    if (!nvs_sync::load_time_off_meta(&s, &e, nullptr, 0)) return false;
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

// Current local time as seconds since midnight. Pass an already-computed
// `tm` (e.g. tick_cb's day-rollover localtime_r()) to skip recomputing it.
static long now_seconds(const struct tm* cached = nullptr) {
    struct tm tm_buf;
    if (!cached) {
        time_t t = time(nullptr);
        localtime_r(&t, &tm_buf);
        cached = &tm_buf;
    }
    return (long)cached->tm_hour * 3600 + cached->tm_min * 60 + cached->tm_sec;
}

// Returns the live meeting list with past meetings removed and in_progress flag
// set. Static backing array — safe in single-threaded LVGL context; do not
// hold past the next call.
static app_state::MeetingList filtered_meetings() {
    static app_state::Meeting buf[32];
    app_state::MeetingList all = { g_rt_display, g_rt_count };
    uint32_t now_epoch = (uint32_t)time(nullptr);
    size_t n = 0;
    // g_rt[i] lines up 1:1 with all.items[i] — all.count is g_rt_count itself
    // (set_meetings_cbor fills both in the same pass, see g_rt_display's decl).
    // List membership and in_progress both key off the same precise epoch
    // fields already computed for each meeting — no reason to also re-derive
    // an end-of-day-minutes value from a formatted "HH:MM" string (as this did
    // previously): that was minute-granularity (up to 59s of lag removing an
    // expired meeting from the list) and did needless string parsing on every
    // 1 s tick for every meeting.
    for (size_t i = 0; i < all.count && n < 32; ++i) {
        const RtMeeting& rt = g_rt[i];
        if (now_epoch <= rt.end_epoch) {
            buf[n] = all.items[i];
            buf[n].in_progress = (now_epoch >= rt.start_epoch);
            ++n;
        }
    }
    return { buf, n };
}

// Scan the runtime meeting list and return true if we just entered a 5-min
// window for any un-alerted meeting. Populates out_* with the meeting data
// if returning true.
static bool check_countdown(const char** out_title,
                             const char** out_org,
                             const char** out_loc,
                             int*         out_diff_s,
                             int*         out_key,
                             const struct tm* now_tm = nullptr) {
    long now_s = now_seconds(now_tm);
    app_state::MeetingList list = { g_rt_display, g_rt_count };

    for (size_t i = 0; i < list.count; ++i) {
        const app_state::Meeting& m = list.items[i];
        int start_mins = hhmm_to_mins(m.start);
        if (start_mins < 0) continue;
        long start_s = (long)start_mins * 60;

        long diff = start_s - now_s;
        if (diff < 0 || diff > ALERT_WINDOW_S) continue;
        if (start_mins >= MINUTES_PER_DAY) continue;  // defensive: keep the array index in bounds
        if (g_alerted_mins[start_mins]) continue;     // already alerted this boot

        *out_title = m.title;
        *out_org   = m.org;
        *out_loc   = m.loc;
        *out_diff_s = (int)diff;
        *out_key = start_mins;
        return true;
    }
    return false;
}

// Apply the shared widget defaults (PC link, mode) before any
// screen is created, so each screen picks up the current runtime state
// without requiring per-screen setup calls. Connection-status border colour
// is NOT re-derived here — unlike weather below, it isn't recomputed from
// cached fields on every rebuild; it's set directly at each transition
// (OrionConnected/SyncBegin/SyncEnd/OrionDisconnected, ble_manager.cpp), and
// widget_profile_card::create() reads the current default for any new card.
void apply_widget_defaults() {
    // Reflect the cached weather condition/temp/unit while the PC link is up
    // and Orion has actually sent weather this boot; hide the icon + text
    // entirely otherwise — there's no "unverified" enum value like Offline
    // to fall back to (ble-protocol.md §6.4).
    widget_profile_card::set_default_weather(
        static_cast<widget_profile_card::WeatherCondition>(g_weather_condition_byte),
        g_weather_temp_f,
        static_cast<widget_profile_card::TemperatureUnit>(g_weather_unit_byte),
        g_pc_connected && g_weather_valid,
        g_weather_is_night,
        static_cast<widget_profile_card::WeatherIntensity>(g_weather_intensity));

    // Mode-toggle is shown once Orion has fully synced OR when in
    // Clock/Calendar (the toggle acts as a "return" button there, works even
    // offline). Gated on g_pc_synced, NOT g_pc_connected — a bonded reconnect
    // sets the latter true immediately on encryption complete, well before
    // Orion has actually resynced (ble-protocol.md §6.2); showing the toggle
    // that early would let the user open Controls mode against stale/
    // incomplete media state mid-resync.
    bool in_clock_like = (g_state == AppState::CLOCK || g_state == AppState::CALENDAR_VIEW);
    bool show_toggle = g_pc_synced || in_clock_like;
    widget_status_bar::set_default_pc_connected(show_toggle);
    widget_status_bar::set_default_phone_bonded(g_phone_bonded);
    widget_status_bar::set_default_phone_connected(g_phone_connected);

    // In Clock/Calendar state the bar shows Mode::Clock so the toggle glyph
    // reads "return to previous mode"; otherwise reflect the current g_mode.
    widget_status_bar::Mode bar_mode;
    if (in_clock_like) {
        bar_mode = widget_status_bar::Mode::Clock;
    } else {
        bar_mode = (g_mode == 1) ? widget_status_bar::Mode::Keyboard
                                 : widget_status_bar::Mode::Calendar;
    }
    widget_status_bar::set_default_mode(bar_mode);

    // Mode-toggle callback: advance mode or exit Clock/Calendar.
    widget_status_bar::set_default_mode_toggle_cb([]() {
        state_machine::on_mode_toggle();
    });

    // Time-tap callback: enter Clock mode.
    widget_status_bar::set_default_time_tap_cb([]() {
        state_machine::on_clock_enter();
    });

    // Time-long-press callback: enter the Calendar (month view).
    widget_status_bar::set_default_time_long_press_cb([]() {
        state_machine::on_calendar_enter();
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

lv_obj_t* build_time_off_screen() {
    return screen_time_off::create();
}

lv_obj_t* build_meeting_list_screen() {
    // "cached" drives the "SYNCED · X min ago" pill. The on-screen list is cached
    // whenever Orion's BLE link is down — a runtime disconnect (meetings still in
    // RAM, clock still valid). While Orion is connected the data is live (sync
    // completes before we leave the reconnect overlay), so no pill. After a power
    // cycle the RAM list is empty, so we're on the "No meetings today" screen
    // instead and the pill never applies. Matches meeting-list.md's offline table.
    return screen_meeting_list::create(filtered_meetings(), !g_pc_connected);
}

lv_obj_t* build_no_meetings_screen() {
    return screen_no_meetings::create();
}

lv_obj_t* build_clock_screen() {
    return (g_clock_face == 1) ? screen_clock_analog::create() : screen_clock::create();
}

lv_obj_t* build_calendar_screen() {
    return screen_calendar::create();
}

lv_obj_t* build_controls_screen() {
    return screen_media_mode::create();
}

lv_obj_t* build_reconnect_screen() {
    return screen_reconnect_syncing::create();
}

// Fire a countdown modal on top of the appropriate base screen.
// The base screen is created fresh (or reused if it's already the current one).
void fire_countdown(const char* title, const char* organizer, const char* location, int diff_s) {
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

    load_screen(base);
    modal_countdown::create(base, title, organizer, location, diff_s);
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
    if (is_time_off_active())        return AppState::TIME_OFF_ACTIVE;
    // Countdown is handled inline in tick() before evaluate() is called for
    // state changes, so if we reach here it has already been cleared.
    // Note: CLOCK and CALENDAR_VIEW are never returned here — they are
    // entered exclusively via on_clock_enter() / on_calendar_enter() (user
    // taps / long-presses the status-bar time) and protected in evaluate()
    // from being overwritten by normal Calendar state changes.

    app_state::MeetingList list = filtered_meetings();
    if (list.count > 0)              return AppState::MEETING_LIST;
    return AppState::NO_MEETINGS;
}

// ─── Periodic tick ────────────────────────────────────────────────────────

// First clock source after a cold boot (Orion Time Sync or iPhone CTS):
// force one rebuild so the current screen repaints with a valid date — in
// particular the "No meetings today" calendar glyph, which falls back to a
// generic grid (no today / no week highlight) while the clock is unset and
// would otherwise stay stale once the time arrives. Excluded during SETUP:
// none of the wizard screens show clock-derived content, and forcing a
// rebuild there tears down and recreates the live setup screen (its own
// step transitions are already driven explicitly by screen_setup::set_step).
// That teardown fires the outgoing screen's DELETE handler, which closes
// the iPhone pairing window (ble_manager::set_iphone_pairing_window(false))
// — since Time Sync typically lands in the same sync that also advances
// Step 3/4 to PhonePairing, this force-rebuild used to slam the just-opened
// pairing window shut a moment later with nothing left to reopen it.
// Returns whether the clock is currently set, for callers later in the same
// tick that also need it.
static bool check_clock_just_set() {
    static bool s_clock_was_set = false;
    bool clock_now = app_state::clock_is_set();
    if (clock_now && !s_clock_was_set && g_state != AppState::SETUP) g_force_rebuild = true;
    s_clock_was_set = clock_now;
    return clock_now;
}

// Day rollover (e.g. past midnight): the calendar glyph and month grid pin
// "today" and the current-week highlight at build time, so refresh when the
// local date advances.
//   • CALENDAR_VIEW — re-render the open month grid in place (keeps the
//     user's navigation; just recomputes the highlight).
//   • CLOCK — nothing to do (it shows the time, not the date).
//   • everything else — force a normal rebuild of the current screen.
// Populates *out_lt with the current local time (when the clock is set) so
// the caller can pass it on to check_meeting_alert() — avoids a second
// redundant time()+localtime_r() in the same 1 Hz callback. Returns whether
// *out_lt was populated.
static bool check_day_rollover(bool clock_now, struct tm* out_lt) {
    static int s_last_day_key = -1;
    bool have_lt = false;
    if (clock_now) {
        time_t now = time(nullptr);
        localtime_r(&now, out_lt);
        have_lt = true;
        int day_key = (out_lt->tm_year + 1900) * 1000 + out_lt->tm_yday;  // unique per calendar day
        if (s_last_day_key != -1 && day_key != s_last_day_key) {
            // New calendar day — clear the per-minute-of-day alert bitmap.
            // Without this the flags lived until reboot, so on this always-on
            // device a recurring meeting (same HH:MM as one that alerted
            // yesterday) NEVER fired its 5-minute countdown again after day
            // one. Keyed by minute-of-day, the bitmap is only meaningful
            // within a single day by construction. No double-fire risk at the
            // boundary: check_countdown()'s window math is same-day-relative
            // (start_s - now_s), so a meeting that alerted late yesterday
            // can't re-enter its window after midnight. A clock JUMP (Orion
            // correcting a badly seeded date) lands here too — also correct:
            // the alert history belonged to a day we were never really in.
            memset(g_alerted_mins, 0, sizeof(g_alerted_mins));
            // Weather Alert is also a once-per-day flag — same day-boundary
            // reasoning as g_alerted_mins above (state-machine.md).
            g_weather_alert_fired_today = false;
            LOG("[sm] day rollover — alert flags cleared\n");
            if (g_state == AppState::CALENDAR_VIEW) {
                screen_calendar::refresh_today();
            } else if (g_state != AppState::CLOCK) {
                g_force_rebuild = true;
            }
        }
        s_last_day_key = day_key;
    }
    return have_lt;
}

// 5-minute pre-meeting alert. Returns true if an alert fired (fire_countdown()
// already rebuilt the screen) — the caller should skip the rest of the tick.
static bool check_meeting_alert(bool have_lt, const struct tm* lt) {
    if (g_state != AppState::COUNTDOWN   &&
        g_state != AppState::SETUP       &&
        g_state != AppState::OTA_UPDATING &&
        !ota_receiver::is_active()       &&
        !is_time_off_active()) {

        const char* title = nullptr;
        const char* org   = nullptr;
        const char* loc   = nullptr;
        int diff_s        = 0;
        int key           = -1;

        if (check_countdown(&title, &org, &loc, &diff_s, &key, have_lt ? lt : nullptr)) {
            if (key >= 0 && key < MINUTES_PER_DAY) g_alerted_mins[key] = true;
            LOG("[sm] countdown alert for meeting key=%d\n", key);
            fire_countdown(title, org, loc, diff_s);
            return true;
        }
    }
    return false;
}

// Meeting content changed under our feet — a meeting crossed into/out of
// "in progress" or expired off the front — without the MEETING_LIST vs
// NO_MEETINGS *AppState* changing (compute_target_state() only tells them
// apart by count>0). evaluate()'s "unchanged" no-op guard would otherwise
// leave a stale in-progress highlight or an expired row on screen until
// some unrelated event forces a rebuild (meeting-list.md's "now crosses
// start → row turns red" / "now crosses end → row is removed" promises
// this happens every tick). Diff against the last-seen snapshot and force
// a rebuild when it doesn't match. Scoped to Calendar mode (g_mode == 0)
// — in Controls/media mode this state renders the media screen instead,
// which has nothing meeting-related to refresh.
static void check_meeting_content_changed(bool clock_now) {
    if (clock_now && g_mode == 0 &&
        (g_state == AppState::MEETING_LIST || g_state == AppState::NO_MEETINGS)) {
        static size_t s_last_count = (size_t)-1;
        static bool   s_last_in_progress[32] = {};
        app_state::MeetingList list = filtered_meetings();
        bool changed = list.count != s_last_count;
        for (size_t i = 0; !changed && i < list.count; ++i) {
            changed = list.items[i].in_progress != s_last_in_progress[i];
        }
        if (changed) {
            s_last_count = list.count;
            for (size_t i = 0; i < list.count; ++i) s_last_in_progress[i] = list.items[i].in_progress;
            g_force_rebuild = true;
        }
    }
}

// Weather Alert / Low Battery Alert trigger-eval (state-machine.md's
// "Weather Alert / Low Battery Alert overlays" section). Building the
// overlay itself is modal_device_alert.cpp; this function ONLY decides
// when to call it — the same split modal_incoming_call.cpp keeps from
// ancs_client.cpp's call-state logic. Neither alert is part of AppState —
// both overlay whatever's currently on screen directly, so this runs
// independently of compute_target_state()/evaluate().
//
// Gated to ALERT_CHECK_TICKS (~60s) — evaluated far less often than the 1 s
// tick that calls it, since both triggers are minute-granularity by nature
// (a work-hours boundary, a battery percentage) and Orion's own mirrored
// reminders poll on roughly the same cadence (pc-app.md).
static void check_device_alerts(bool have_lt, const struct tm* lt) {
    static uint32_t s_alert_check_ticks = 0;
    if (++s_alert_check_ticks < ALERT_CHECK_TICKS) return;
    s_alert_check_ticks = 0;

    // Weather Alert: needs a valid local clock (work-day/time-of-day math)
    // and Orion's live weather condition — g_weather_valid gates this the
    // same "don't show what can't be verified" way the profile card's own
    // icon does (cleared on PC disconnect, set_pc_connected() above).
    if (have_lt && g_weather_valid && !g_weather_alert_fired_today &&
        nvs::get_weather_alert_enabled()) {
        using WC = widget_profile_card::WeatherCondition;
        auto cond = static_cast<WC>(g_weather_condition_byte);
        bool is_rain_snow_storm = (cond == WC::Rain || cond == WC::Thunderstorm || cond == WC::Snow);

        if (is_rain_snow_storm) {
            uint8_t day_mask   = nvs::get_work_hours_days();
            int     wday_bit   = (lt->tm_wday + 6) % 7;  // tm_wday: Sun=0..Sat=6 -> bit0=Mon..bit6=Sun
            bool    is_work_day = (day_mask & (1 << wday_bit)) != 0;

            if (is_work_day) {
                int end_min = (int)nvs::get_work_hours_end_min();
                int offset  = (int)nvs::get_weather_alert_offset_min();
                int now_min = lt->tm_hour * 60 + lt->tm_min;
                if (now_min >= end_min - offset && now_min < end_min) {
                    char end_str[16];
                    time_format::hhmm(end_str, sizeof(end_str), end_min / 60, end_min % 60);
                    modal_device_alert::show_weather_alert(cond, end_str);
                    g_weather_alert_fired_today = true;
                    LOG("[sm] weather alert fired (condition=%u, end=%s)\n",
                        (unsigned)g_weather_condition_byte, end_str);
                }
            }
        }
    }

    // Low Battery Alert: needs the iPhone actually connected —
    // ancs_client::phone_stats() is all-zero otherwise (same "don't show
    // what can't be verified" policy, ble-protocol.md).
    if (g_phone_connected && nvs::get_low_battery_alert_enabled()) {
        ancs_client::PhoneStats stats = ancs_client::phone_stats();
        uint8_t threshold = nvs::get_low_battery_threshold_pct();
        if (!g_low_battery_alert_latched && stats.battery <= threshold) {
            modal_device_alert::show_low_battery_alert(
                ancs_client::phone_kind_word(), ancs_client::phone_name(), stats.battery);
            g_low_battery_alert_latched = true;
            LOG("[sm] low battery alert fired (battery=%u%%)\n", (unsigned)stats.battery);
        } else if (g_low_battery_alert_latched && stats.battery > threshold) {
            // Recovered above the threshold — un-latch so a future dip fires again.
            g_low_battery_alert_latched = false;
        }
    }
}

static void tick_cb(lv_timer_t* /*t*/) {
    bool clock_now = check_clock_just_set();

    struct tm lt;
    bool have_lt = check_day_rollover(clock_now, &lt);

    if (check_meeting_alert(have_lt, &lt)) return;  // screen already updated; skip re-evaluate below

    check_meeting_content_changed(clock_now);

    check_device_alerts(have_lt, &lt);

    state_machine::evaluate();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

namespace state_machine {

void init() {
    // Pre-load Time Off metadata and sync hashes into RAM cache while the heap
    // is clean (before any screen is created). This prevents the NVS handle
    // allocator from landing on SRAM that was previously used by the media
    // screen's LVGL allocations, which would corrupt the NVSHandleSimple
    // vtable pointer and crash.
    nvs_sync::prime_time_off_cache();
    nvs_sync::prime_hash_cache();

    // Restore mode, clock-face preference, and ANCS filter from NVS.
    g_mode = nvs::get_mode();
    g_clock_face = nvs::get_clock_face();
    ancs_client::set_filter(nvs::get_notif_filter());

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
    // Hold the Setup-Complete screen through its checkmark + linger. Cleared by
    // on_setup_complete() (the 5 s timer) which then transitions to runtime.
    if (g_setup_complete_hold) return g_state;

    AppState target = compute_target_state();

    // COUNTDOWN: user must tap Close; not overridden by normal state changes.
    if (g_state == AppState::COUNTDOWN && target != AppState::SETUP) {
        return g_state;
    }

    // CLOCK / CALENDAR_VIEW: user-entered by tapping/long-pressing the time;
    // persists through normal Calendar state changes (meeting list updates,
    // Time Off becoming/staying active, etc.). Only a full-screen takeover
    // (OTA / Setup) or an explicit on_mode_toggle() (which forces a rebuild)
    // can pull the user out — a passive state change like Time Off must not
    // yank away a view the user explicitly opened.
    bool force = g_force_rebuild;
    g_force_rebuild = false;
    if (!force &&
        (g_state == AppState::CLOCK || g_state == AppState::CALENDAR_VIEW) &&
        target != AppState::SETUP &&
        target != AppState::OTA_UPDATING) {
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

        case AppState::TIME_OFF_ACTIVE:
            new_screen = build_time_off_screen();
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

        case AppState::CALENDAR_VIEW:
            new_screen = build_calendar_screen();
            break;
    }

    if (new_screen) {
        load_screen(new_screen);
        LOG("[sm] state -> %d  mode=%d\n", (int)g_state, (int)g_mode);
    }

    return g_state;
}

void notify_time_off_image_changed() {
    // The Time Off image arrives asynchronously, after the synchronous meetings
    // commit has already rebuilt the panel with the placeholder. If Time Off is the
    // screen on display right now, force one rebuild so screen_time_off::create()
    // re-reads photo_cache::get_time_off() and shows the real destination image
    // (or reverts to the placeholder if the image was cleared). When Time Off isn't
    // the current screen the next entry into TIME_OFF_ACTIVE picks up the cached
    // image on its own, so this is a no-op.
    if (g_state == AppState::TIME_OFF_ACTIVE) {
        g_force_rebuild = true;
        evaluate();
    }
}

// Deferred work flags — set from LVGL timer callbacks, drained in poll()
// which runs in the main loop before lv_timer_handler(). NVS flash writes
// disable ICache/DCache system-wide; doing them inside an LVGL callback
// while LCD_CAM DMA is active on Core 1 triggers the interrupt watchdog.
static bool g_setup_complete_pending = false;
static bool g_unpair_phone_pending   = false;
static bool g_phone_wipe_pending     = false;  // stale-bond wipe before re-pair
static bool g_mode_write_pending     = false;  // persist g_mode (set from on_mode_toggle)

// Copy a CBOR text string into a fixed buffer, truncating cleanly on a UTF-8
// character boundary when it's longer than the buffer. Plain
// cbor_value_copy_text_string() copies NOTHING when the buffer is too small (it
// returns CborErrorOutOfMemory and leaves the buffer untouched), which would
// render an over-length meeting title/location as BLANK. This keeps the prefix
// that fits, never splitting a multi-byte UTF-8 sequence.
static void copy_text_truncated(const CborValue* field, char* dst, size_t dst_sz) {
    if (dst_sz == 0) return;
    dst[0] = '\0';
    size_t sz = dst_sz - 1;
    CborError err = cbor_value_copy_text_string(field, dst, &sz, nullptr);
    if (err == CborNoError) { dst[sz] = '\0'; return; }   // fit exactly
    if (err != CborErrorOutOfMemory) return;              // malformed → leave empty
    // Too long: `sz` now holds the full byte length. Copy the whole string into
    // a temp buffer, then keep the longest UTF-8-valid prefix that fits.
    size_t full = sz;
    char* tmp = static_cast<char*>(malloc(full + 1));
    if (!tmp) return;                                     // OOM → leave empty
    size_t tsz = full + 1;
    if (cbor_value_copy_text_string(field, tmp, &tsz, nullptr) == CborNoError) {
        size_t n = dst_sz - 1;
        while (n > 0 && ((uint8_t)tmp[n] & 0xC0) == 0x80) --n;  // don't split a char
        memcpy(dst, tmp, n);
        dst[n] = '\0';
    }
    free(tmp);
}

void set_meetings_cbor(const uint8_t* buf, size_t len) {
    if (!buf || !len) return;

    // Meetings are RAM-only (see header): no NVS write here.

    // Parse CBOR: { "d":date, "m":[ { "i":id, "s":start, "e":end,
    //   "t":title, "l":loc, "o":org }, ... ] }
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

        if (strcmp(key, "m") == 0 && cbor_value_is_array(&map)) {
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

            if (strcmp(fkey, "s") == 0 && cbor_value_is_unsigned_integer(&field)) {
                uint64_t v; cbor_value_get_uint64(&field, &v);
                rt.start_epoch = (uint32_t)v;
                epoch_to_hhmm(rt.start_epoch, rt.start_str, sizeof(rt.start_str));
            } else if (strcmp(fkey, "e") == 0 && cbor_value_is_unsigned_integer(&field)) {
                uint64_t v; cbor_value_get_uint64(&field, &v);
                rt.end_epoch = (uint32_t)v;
                epoch_to_hhmm(rt.end_epoch, rt.end_str, sizeof(rt.end_str));
            } else if (strcmp(fkey, "t") == 0 && cbor_value_is_text_string(&field)) {
                copy_text_truncated(&field, rt.title, sizeof(rt.title));
            } else if (strcmp(fkey, "l") == 0 && cbor_value_is_text_string(&field)) {
                copy_text_truncated(&field, rt.loc, sizeof(rt.loc));
            } else if (strcmp(fkey, "o") == 0 && cbor_value_is_text_string(&field)) {
                copy_text_truncated(&field, rt.org, sizeof(rt.org));
            }
            if (!cbor_value_at_end(&field)) cbor_value_advance(&field);
        }
        // cbor_value_leave_container() already advances `item` past this
        // meeting's map onto the next array element (or end-of-array) — an
        // extra cbor_value_advance(&item) here double-advances, silently
        // skipping every other meeting on even-length lists and asserting
        // (advancing past an already-exhausted iterator) on odd-length ones.
        cbor_value_leave_container(&item, &field);

        // Drop unrenderable glyphs (emoji, CJK, …) from the displayed fields.
        sanitize_inplace(rt.title, sizeof(rt.title));
        sanitize_inplace(rt.loc,   sizeof(rt.loc));
        sanitize_inplace(rt.org,   sizeof(rt.org));

        // Build the app_state::Meeting view.
        g_rt_display[count] = {
            rt.start_str, rt.end_str,
            rt.title, rt.loc, rt.org,
            false,  // overlap — computed below
            false   // in_progress — computed in filtered_meetings()
        };
        ++count;
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

// Meetings are RAM-only — there is no boot-time load from NVS. After a power
// cycle the meeting list is empty until Orion reconnects and re-pushes it
// (see set_meetings_cbor / meeting-list.md). The local clock is likewise not
// restored from flash, so the meeting time logic can't run offline anyway.

void hold_for_setup_complete() {
    // Called from build_complete() when the Setup-Complete screen appears.
    g_setup_complete_hold = true;
}

void on_setup_complete() {
    LOG("[sm] on_setup_complete — deferring NVS write to poll()\n");
    g_setup_complete_hold    = false;  // release the hold — hand off to runtime now
    g_setup_complete_pending = true;
}

void poll() {
    if (g_setup_complete_pending) {
        g_setup_complete_pending = false;
        LOG("[sm] poll: setup complete — writing NVS + evaluating\n");
        nvs::mark_setup_complete();
        evaluate();
        // Now that the Setup Complete screen's own animations are done and the
        // runtime screen is up, run any deferred ANCS backlog-flush reconnect
        // (ble_manager.h) — see its doc comment for why this is deferred here
        // rather than fired on a short timer right after the iPhone bonded.
        ble_manager::run_pending_ancs_backlog_reconnect();
    }
    if (g_unpair_phone_pending) {
        g_unpair_phone_pending = false;
        LOG("[sm] poll: unpair phone — wiping bond + evaluating\n");
        // wipe_iphone_bond() owns advertising: it stops it during the wipe and
        // restarts it once the bond is deleted (deferred to the disconnect if
        // the iPhone is still connected). Do NOT restart advertising here — that
        // let the still-bonded iPhone reconnect and race the wipe (NVS crash).
        ble_manager::wipe_iphone_bond();
        g_force_rebuild = true;
        g_state = AppState::NO_MEETINGS;  // break out of any early-return guard
        evaluate();
    }
    if (g_phone_wipe_pending) {
        g_phone_wipe_pending = false;
        LOG("[sm] poll: wiping stale iPhone bond for re-pair\n");
        // No evaluate() — the re-pair screen the user just opened owns the
        // display. wipe_iphone_bond() re-advertises after the wipe so the
        // now-empty iPhone slot + open pairing window put ANCS solicitation on air.
        ble_manager::wipe_iphone_bond();
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
    if (g_state == AppState::CLOCK || g_state == AppState::CALENDAR_VIEW) {
        // In Clock/Calendar: return to the mode that was active before the
        // time tap/long-press.
        g_mode = g_pre_clock_mode;
        LOG("[sm] clock/calendar exit -> mode=%d\n", (int)g_mode);
        g_force_rebuild = true;
        g_state = AppState::NO_MEETINGS;  // break Clock/Calendar protection in evaluate()
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

void on_calendar_enter() {
    if (g_state == AppState::CALENDAR_VIEW) return;  // already in Calendar, no-op
    g_pre_clock_mode = g_mode;
    g_state = AppState::CALENDAR_VIEW;
    screen_calendar::reset_view();  // always open on the current month
    apply_widget_defaults();
    load_screen(build_calendar_screen());
    LOG("[sm] calendar enter (pre_mode=%d)\n", (int)g_pre_clock_mode);
}

bool show_countdown_if_imminent(const app_state::Meeting& m) {
    int start_mins = hhmm_to_mins(m.start);
    if (start_mins < 0) return false;

    long diff = (long)start_mins * 60 - now_seconds();
    if (diff < 0 || diff > ALERT_WINDOW_S) return false;  // not within the 5-min window

    // Mark it alerted so the automatic tick-driven alert doesn't also pop up
    // moments later for a meeting the user just looked at directly.
    if (start_mins < MINUTES_PER_DAY) g_alerted_mins[start_mins] = true;

    LOG("[sm] countdown opened by tap (meeting key=%d)\n", start_mins);
    fire_countdown(m.title, m.org, m.loc, (int)diff);
    return true;
}

void on_countdown_close() {
    if (g_state != AppState::COUNTDOWN) return;  // already superseded (OTA, factory reset, ...)
    // Don't call evaluate() synchronously from here — this runs from inside
    // the countdown modal's own Close click / self-dismiss timer, both of
    // which are children of the screen evaluate() would tear down. Just clear
    // the sentinel; the tick already calls evaluate() once a second, and the
    // modal (built fresh at fire_countdown() time) already reflects the
    // meeting state accurately in the meantime.
    g_state = AppState::NO_MEETINGS;
    g_force_rebuild = true;
    LOG("[sm] countdown closed\n");
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

void register_runtime_calendar(lv_obj_t* screen, lv_obj_t* body, lv_obj_t* left) {
    g_runtime_screen = screen;
    g_runtime_body   = body;
    g_runtime_left   = left;
    // Clear the pointers when THIS screen is deleted — guarded by identity so
    // the normal load order (new screen registers itself, THEN the old screen's
    // delete fires) can't have the outgoing screen's delete null out the
    // incoming screen's freshly-registered pointers.
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        if (static_cast<lv_obj_t*>(lv_event_get_target(e)) == g_runtime_screen) {
            g_runtime_screen = nullptr;
            g_runtime_body   = nullptr;
            g_runtime_left   = nullptr;
        }
    }, LV_EVENT_DELETE, nullptr);
}

// Rebuilds ONLY the left panel of the live calendar-runtime screen in place —
// the shared status bar + profile card (which already updated their own
// contents live during the sync) are never touched, so there's no full-screen
// teardown, no re-layout, and no status-bar-timer churn. Picks meeting-list vs
// no-meetings from the freshly-synced data, exactly as compute_target_state()
// would. No-op if no calendar screen is live.
static void refresh_runtime_left() {
    if (!g_runtime_body) return;
    app_state::MeetingList list = filtered_meetings();
    // Built as body's LAST child; move to index 0 so the body's flex order
    // stays [left, divider, profile card], then drop the old left panel.
    lv_obj_t* new_left = (list.count > 0)
        ? screen_meeting_list::build_left(g_runtime_body, list, !g_pc_connected)
        : screen_no_meetings::build_left(g_runtime_body);
    lv_obj_move_to_index(new_left, 0);
    if (g_runtime_left) lv_obj_delete(g_runtime_left);
    g_runtime_left = new_left;
}

// Shared tail of on_reconnect_end() — split out so the RECONNECT_MIN_MS
// minimum-visible deferral can invoke the exact same completion logic from its
// timer callback. Fires on EVERY SyncEnd (a reconnect resync, a user-initiated
// profile/photo/Time Off push, AND a tiny periodic time-sync) — so it must be
// cheap in the common case and only do real work when something visible
// actually changed.
static void finish_reconnect_now() {
    bool was_overlay          = g_reconnect_overlay_mode;
    bool was_reconnect_screen = (g_state == AppState::RECONNECT_SYNCING);
    // Consumed exactly once here, regardless of which branch below actually
    // ends up needing it — see mark_display_needs_repaint()'s doc comment
    // (state_machine.h) for why this is a separate flag from was_overlay.
    bool needs_repaint        = g_needs_repaint;
    g_needs_repaint           = false;
    // ota_receiver::dismiss_error() ("Close" on the Update-failed screen)
    // flips g_ota_state to Idle BEFORE routing through on_reconnect_end() into
    // here, specifically so this is distinguishable from a background BLE sync
    // that happens to complete WHILE the Update-failed screen is still up
    // (gatt_server::set_ota_active(false) already ran when the error screen
    // appeared, so BLE data writes aren't NACKed during it) — that case must
    // leave the error screen alone until the user actually taps Close.
    // on_ota_begin()/ota_show() did a full-screen takeover (load_screen()),
    // which tears down the runtime screen and nulls g_runtime_body via its
    // delete callback, so neither branch below would otherwise ever fire for
    // a real dismiss (only the debug-serial trigger, which uses a separate
    // unconditional rebuild path, exercised the Close button correctly).
    bool leaving_ota = (g_state == AppState::OTA_UPDATING) &&
                        !ota_receiver::is_active();
    g_reconnect_overlay_mode  = false;
    screen_reconnect_syncing::destroy_overlay();

    // Mirror the guard in on_reconnect_begin(): a sync that ran while the
    // countdown modal was up never touched g_state on the way in, so don't
    // touch it on the way out either — the modal stays exactly as it is until
    // the user taps Close. Still repaints on the way out if this sync's
    // commit blanked the framebuffer (same gap as below, just for this
    // screen) — the countdown ring/text are otherwise left blank too.
    if (g_state == AppState::COUNTDOWN) {
        if (needs_repaint) lv_obj_invalidate(lv_scr_act());
        return;
    }

    if (g_runtime_body) {
        // A live calendar screen (NO_MEETINGS / MEETING_LIST) is on top → never
        // a full-screen rebuild. Set the real target first so the next
        // evaluate() tick no-ops (compute_target_state early-returns
        // RECONNECT_SYNCING while that's the state, so it must be cleared here).
        app_state::MeetingList list = filtered_meetings();
        g_state = (list.count > 0) ? AppState::MEETING_LIST : AppState::NO_MEETINGS;
        if (was_overlay) {
            // Big sync (photo / profile / meetings / Time Off): rebuild ONLY the
            // left panel in place — the shared status bar + profile card, whose
            // own contents already updated live during the sync (set_photo /
            // set_profile), are left untouched. Then repaint the
            // screen to recover from the NVS-commit blackout that blanked the
            // whole framebuffer (run_staged_commit, ble-protocol.md §6.0) — a
            // redraw of the existing widget tree, NOT a rebuild: no allocation,
            // no re-layout, no status-bar-timer churn.
            refresh_runtime_left();
            lv_obj_invalidate(lv_scr_act());
        } else if (needs_repaint) {
            // A small mid-session sync (e.g. a profile-only edit, or removing
            // the profile photo) never shows the overlay — its declared
            // total_bytes can easily land under RECONNECT_OVERLAY_MIN_BYTES —
            // but still commits to NVS and blanks the framebuffer exactly like
            // a big sync does. Nothing about the meeting list itself changed
            // (this push carries no Meetings/Time Off data), so skip the
            // rebuild, but the blanked screen still needs an explicit repaint
            // or it's left showing black/empty indefinitely: LVGL's object
            // tree is unchanged, so nothing else would ever trigger one.
            lv_obj_invalidate(lv_scr_act());
        }
        // Otherwise (neither flag set) this was a tiny periodic sync that
        // touched no flash (time-only). Nothing visible changed and nothing
        // was blanked, so leave the live meeting list exactly as it is: no
        // rebuild, no repaint, no flicker.
        return;
    }

    // No live calendar screen to refresh in place. Only rebuild if a reconnect
    // SCREEN is actually up — the full-screen fallback shown when the sync began
    // while in Clock/Calendar/media, or a calendar screen that was replaced
    // mid-sync — or we're actually leaving an OTA takeover (see leaving_ota
    // above). A tiny periodic sync arriving while in Clock/Calendar, or one
    // that completes in the background while Update-failed is still shown,
    // reaches here with both flags false and is correctly left alone (unlike
    // the old unconditional rebuild, which yanked the user out of Clock).
    if (was_reconnect_screen || leaving_ota) {
        g_force_rebuild = true;
        g_state = AppState::NO_MEETINGS;
        evaluate();
    } else if (needs_repaint) {
        // Same NVS-blackout-with-no-overlay case as above, but for whatever
        // non-calendar screen (Clock, Calendar month view, Controls mode)
        // happened to be up when this small sync's commit blanked the whole
        // framebuffer — it needs the same explicit repaint, not a rebuild.
        lv_obj_invalidate(lv_scr_act());
    }
}

void on_reconnect_begin() {
    // Never overlay the reconnect-syncing UI on top of an OTA takeover.
    if (ota_receiver::is_active()) return;
    // Never preempt the 5-minute countdown modal — it's priority 3, strictly
    // above Reconnect-Syncing's priority 4 (state-machine.md), and must persist
    // until the user taps Close. Let the sync run silently underneath (same as
    // a tiny periodic sync arriving during Clock/Calendar); finish_reconnect_now()
    // has a matching guard so on_reconnect_end() doesn't clobber g_state back
    // out of COUNTDOWN either. Without this, a resync landing inside the
    // 5-minute window would tear down the countdown screen entirely — and
    // since the meeting's alerted flag is already set, the alert would never
    // come back.
    if (g_state == AppState::COUNTDOWN) return;
    // Idempotent — guards against being called twice for the same reconnect.
    if (g_state == AppState::RECONNECT_SYNCING) return;
    LOG("[sm] on_reconnect_begin (meetings cached=%u)\n", (unsigned)g_rt_count);
    // Shown on every reconnect/resync — including the very first sync after a
    // fresh boot (meetings RAM-only, so g_rt_count==0 then). Profile, Photo,
    // and Time Off are NVS-backed and can be large enough that the sync takes a
    // while AND ends in a display blackout for the flash commit
    // (ble-protocol.md §6.0) — without this the user would otherwise sit on a
    // static "No meetings today" screen and then have it go black with no
    // explanation. The progress ring is driven by real byte progress from
    // BEGIN onward (ble_manager's OrioningProgress fan-out).
    g_reconnect_shown_ms = millis();

    // Preferred path: if a live calendar-runtime screen is on top, mask ONLY
    // its left panel with the sync ring — leaving the status bar + profile card
    // exactly as they are (no teardown, no reflow, no timer churn). This is the
    // common case (reconnecting from the normal meeting/no-meetings screen) and
    // the whole point of this design. The profile card keeps updating its photo/
    // name/connection-status live underneath (set_photo / set_profile /
    // set_default_conn_status), so nothing about the surrounding chrome flashes.
    if (g_runtime_left &&
        (g_state == AppState::NO_MEETINGS || g_state == AppState::MEETING_LIST)) {
        g_reconnect_overlay_mode = true;
        g_state = AppState::RECONNECT_SYNCING;
        screen_reconnect_syncing::create_overlay(g_runtime_left);
        return;
    }

    // Fallback: no calendar left panel to overlay (reconnecting from Clock/
    // Calendar/media, or first screen). Use the standalone reconnect screen.
    g_reconnect_overlay_mode = false;
    g_state = AppState::RECONNECT_SYNCING;
    apply_widget_defaults();
    load_screen(build_reconnect_screen());
}

void on_reconnect_end() {
    LOG("[sm] on_reconnect_end\n");

    // Guaranteed-late second ANCS relay resync (char 0010/0011,
    // ble-protocol.md §13). The FIRST resync attempt is Ori's own
    // onSubscribe()-triggered one (ancs_client's resync_orion_* functions,
    // fired from gatt_server.cpp's onSubscribe()) — but on a BONDED
    // RECONNECT that one is a race Orion structurally cannot win: NimBLE's
    // own host stack (ble_gatts_bonding_restored(), nimble/host/src/
    // ble_gatts.c) restores every persisted CCCD for the reconnecting peer
    // and re-fires the subscribe event — and thus this resync — the instant
    // encryption is restored, entirely server-side, with NO new CCCD write
    // from the central required. That happens before Orion has necessarily
    // even finished GATT service discovery, let alone subscribed — so no
    // amount of Orion subscribing earlier can reliably beat it (confirmed
    // 2026-07 via hardware log: Ori's onSubscribe/resync fires and sends
    // "clear" + every queued notification correctly, but Orion's receiver
    // — created as early as its OWN code allows — still isn't alive yet,
    // and the burst is lost for the rest of that connection). This point
    // (on_reconnect_end, fired only on a genuine reconnect's SyncEnd, never
    // on first pairing) is causally guaranteed to be AFTER Orion's own
    // subscribe: Orion sends SyncControl{END} — what triggers this — only
    // as the last step of run_sync(), which its Rust code always runs
    // strictly after subscribing to these two characteristics. Both resync
    // functions are idempotent (a full clear + re-add of whatever's
    // currently queued/live, same as an ordinary filter-change resync) —
    // firing them again here on top of a first resync that DID land is a
    // harmless no-op-equivalent refresh, not a correctness risk.
    ancs_client::resync_orion_relay();
    ancs_client::resync_orion_call_state();

    // Resume the meeting-check tick if an OTA had paused it (no-op otherwise —
    // this is also the normal BLE reconnect-overlay exit). dismiss_error() routes
    // a failed/aborted OTA here.
    if (g_tick_timer) lv_timer_resume(g_tick_timer);

    // poll() drains the BLE event queue in a single while-loop, so on_reconnect_end()
    // can fire in the same drain as on_reconnect_begin() — before LVGL renders even
    // one frame of the overlay. Enforce a minimum visible duration by deferring
    // completion when the overlay was shown too recently.
    if (g_state == AppState::RECONNECT_SYNCING && g_reconnect_shown_ms) {
        uint32_t elapsed = millis() - g_reconnect_shown_ms;
        if (elapsed < RECONNECT_MIN_MS) {
            lv_timer_create([](lv_timer_t* t) {
                lv_timer_delete(t);
                finish_reconnect_now();
            }, RECONNECT_MIN_MS - elapsed, nullptr);
            return;
        }
    }

    finish_reconnect_now();
}

void mark_display_needs_repaint() {
    g_needs_repaint = true;
}

void set_weather(uint8_t condition, int16_t temp_f, uint8_t unit,
                  bool is_night, uint8_t intensity) {
    g_weather_condition_byte = condition;
    g_weather_temp_f = temp_f;
    g_weather_unit_byte = unit;
    g_weather_is_night = is_night;
    g_weather_intensity = intensity;
    g_weather_valid = true;
    widget_profile_card::set_default_weather(
        static_cast<widget_profile_card::WeatherCondition>(g_weather_condition_byte),
        g_weather_temp_f,
        static_cast<widget_profile_card::TemperatureUnit>(g_weather_unit_byte),
        g_pc_connected && g_weather_valid,
        g_weather_is_night,
        static_cast<widget_profile_card::WeatherIntensity>(g_weather_intensity));
}

void set_pc_connected(bool connected) {
    bool changed = (connected != g_pc_connected);
    g_pc_connected = connected;
    // Every (re)connect or disconnect starts (or ends) "not yet synced" —
    // only a real SyncControl{END} (set_pc_synced(), from handle_sync_end())
    // flips this back on. Keeps the mode-toggle hidden through a bonded
    // reconnect's RECONNECTING/SYNCING window, not just while disconnected.
    g_pc_synced = false;

    if (!connected) {
        // "don't show what can't be verified" rule for weather — hide
        // the badge/bubble on the next apply_widget_defaults() rebuild
        // (triggered below), but keep the cached condition/temp numbers;
        // only the visibility gate needs to reset.
        g_weather_valid = false;
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
    //
    // Also skip the rebuild during first-boot setup: the setup flow owns the
    // screen (driven by set_step + the passkey/orioning modals on BLE events),
    // and build_setup_screen() re-derives the step from NVS flags that aren't
    // set until the bond handshake commits. Rebuilding on the *provisional*-bond
    // OrionConnected event (before mark_orion_bonded → is_awaiting_sync) would
    // snap Step 2 "Connect on Orion" back to Welcome. The mode-toggle this
    // rebuild serves is a runtime-only affordance anyway.
    if (changed && !ota_receiver::is_active() && !nvs::is_first_boot()) {
        g_force_rebuild = true;
        g_state = AppState::NO_MEETINGS;  // ensure evaluate() does a full rebuild
        evaluate();
    }
}

void set_pc_synced() {
    bool changed = !g_pc_synced;
    g_pc_synced = true;
    // Same OTA/first-boot rebuild guards as set_pc_connected() above, PLUS a
    // "changed" gate: handle_sync_end() also fires for small periodic
    // in-session refreshes (Time Sync every 10 min, meeting list every 15
    // min — ble-protocol.md §6.3) while already synced. Without this gate,
    // every one of those would force a full screen rebuild for no visible
    // change — exactly what state-machine.md's "periodic refreshes ...
    // do not trigger the reconnecting overlay" is designed to avoid.
    if (changed && !ota_receiver::is_active() && !nvs::is_first_boot()) {
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
    // Same authoritative signal drives an already-open iPhone Info modal's
    // connection dot/label/name/badges, if one happens to be open — no-op
    // otherwise. Every caller of this function reaches it from a plain
    // function-call chain (ble_manager::poll()'s event drain), never from
    // inside an LVGL callback, so it's safe to rebuild synchronously here.
    modal_iphone_info::set_connected(connected);
    if (connected) {
        // Connecting implies a bond now exists — persist the flag and update the
        // active bar immediately so tapping the icon goes to the unpair modal
        // rather than the pairing screen.
        g_phone_bonded = true;
        widget_status_bar::set_all_phone_bonded(true);
        // Low Battery Alert latch clears on the phone reconnecting, not just
        // on recovering above the threshold — mirrors pc-app.md's
        // checkLowBattery() semantics exactly (state-machine.md).
        g_low_battery_alert_latched = false;
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

void set_clock_face(uint8_t face) {
    g_clock_face = face;
    nvs::set_clock_face(face);
    LOG("[sm] clock_face -> %s\n", face ? "Analog" : "Digital");
    // If the Clock state is currently on screen, rebuild it now with the new
    // face. Called from serial-poll context (not an LVGL timer), so an NVS
    // write here is safe — see the on_mode_toggle() deferred-write comment.
    if (g_state == AppState::CLOCK) {
        load_screen(build_clock_screen());
    }
}

uint8_t current_clock_face() {
    return g_clock_face;
}

void set_time_format(uint8_t fmt) {
    time_format::set(fmt);
    LOG("[sm] time_format -> %s\n", fmt ? "12h" : "24h");
    // The status bar and both clock faces reformat on their own 1 s timers, but
    // the meeting list is static once built, so force a rebuild of the mode-driven
    // screen. CLOCK is rebuilt explicitly (a forced evaluate() would switch away
    // from it, since compute_target_state() never returns CLOCK); CALENDAR_VIEW
    // shows no time-of-day and must not be yanked out from under the user.
    if (g_state == AppState::CLOCK) {
        load_screen(build_clock_screen());
    } else if (g_state != AppState::CALENDAR_VIEW) {
        g_force_rebuild = true;
        evaluate();
    }
}

} // namespace state_machine
