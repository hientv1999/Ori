#include "state_machine.h"

#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include <string>
#include <set>
#include <vector>

#include "factory_reset.h"
#include "mock_data.h"
#include "nvs_store.h"
#include "screens/modal_countdown.h"
#include "screens/modal_factory_reset.h"
#include "screens/modal_unpair_phone.h"
#include "screens/screen_clock.h"
#include "screens/screen_keyboard_mode.h"
#include "screens/screen_meeting_list.h"
#include "screens/screen_no_meetings.h"
#include "screens/screen_ota_updating.h"
#include "screens/screen_pto.h"
#include "screens/screen_reconnect_syncing.h"
#include "screens/screen_repair_phone.h"
#include "screens/screen_setup.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Ori — State machine.
//
// Left-panel priority logic per state-machine.md. 1 s tick drives the
// work-hour boundary, 5-min countdown, and meeting-list refresh.

namespace {

// ─── Constants ────────────────────────────────────────────────────────────

constexpr int WORK_HOUR_START   = 8;   // 08:00 local
constexpr int WORK_HOUR_END     = 17;  // 17:00 local
constexpr int ALERT_WINDOW_S    = 300; // 5 minutes in seconds
constexpr uint32_t TICK_MS      = 1000;

// ─── Module state ─────────────────────────────────────────────────────────

AppState g_state        = AppState::CLOCK;     // current rendered state
uint8_t  g_mode         = 0;                   // 0 = Calendar, 1 = Controls
bool     g_pc_connected = true;
bool     g_phone_connected = false;

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

// ─── Helpers ──────────────────────────────────────────────────────────────

bool is_work_hours() {
    time_t now_t = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now_t, &tm_buf);
    int h = tm_buf.tm_hour;
    return (h >= WORK_HOUR_START && h < WORK_HOUR_END);
}

bool is_pto_active() {
    // Stubbed until M5 provides the NVS-cached PTO entry with epoch timestamps.
    return false;
}

// Convert a "HH:MM" string from mock_data to minutes-since-midnight.
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

// Returns mock_data::meetings() with any meeting whose end time has already
// passed (end_minutes < now_minutes) removed. Uses a static backing array —
// safe in single-threaded LVGL context; callers must not hold the returned
// pointer past the next call to this function.
static mock_data::MeetingList filtered_meetings() {
    static mock_data::Meeting buf[32];
    mock_data::MeetingList all = mock_data::meetings();
    int now_m = now_mins();
    size_t n = 0;
    for (size_t i = 0; i < all.count && n < 32; ++i) {
        int end_m = hhmm_to_mins(all.items[i].end);
        if (end_m < 0 || end_m >= now_m) {
            buf[n++] = all.items[i];
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
    mock_data::MeetingList list = mock_data::meetings();

    for (size_t i = 0; i < list.count; ++i) {
        const mock_data::Meeting& m = list.items[i];
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
    widget_profile_card::set_default_presence(
        g_pc_connected
            ? widget_profile_card::Presence::Available
            : widget_profile_card::Presence::Offline);

    widget_status_bar::set_default_pc_connected(g_pc_connected);
    widget_status_bar::set_default_phone_bonded(false);

    widget_status_bar::Mode mode = (g_mode == 1)
        ? widget_status_bar::Mode::Keyboard
        : widget_status_bar::Mode::Calendar;
    widget_status_bar::set_default_mode(mode);

    // Mode-toggle callback: flip mode and re-evaluate.
    widget_status_bar::set_default_mode_toggle_cb([]() {
        state_machine::on_mode_toggle();
    });
}

// Transition to a new LVGL screen, deleting the old one.
void load_screen(lv_obj_t* new_screen) {
    lv_obj_t* prev = g_current_screen;
    g_current_screen = new_screen;
    lv_scr_load(new_screen);
    lv_refr_now(lv_display_get_default());
    if (prev && prev != new_screen) {
        lv_obj_delete(prev);
    }
}

// ─── State-specific screen builders ───────────────────────────────────────

lv_obj_t* build_setup_screen() {
    return screen_setup::create(screen_setup::Step::Welcome);
}

lv_obj_t* build_ota_screen() {
    return screen_ota_updating::create();
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
    return screen_keyboard_mode::create();
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
    } else if (is_work_hours()) {
        mock_data::MeetingList list = filtered_meetings();
        if (list.count > 0) {
            base = build_meeting_list_screen();
        } else {
            base = build_no_meetings_screen();
        }
    } else {
        base = build_clock_screen();
    }

    g_countdown_base = base;
    load_screen(base);
    modal_countdown::create(base, title, when, diff_s);
    g_state = AppState::COUNTDOWN;
}

// ─── Priority evaluator ───────────────────────────────────────────────────

AppState compute_target_state() {
    if (nvs::is_first_boot())        return AppState::SETUP;
    if (g_state == AppState::OTA_UPDATING) return AppState::OTA_UPDATING;
    if (g_state == AppState::RECONNECT_SYNCING) return AppState::RECONNECT_SYNCING;
    if (is_pto_active())             return AppState::PTO_ACTIVE;
    // Countdown is handled inline in tick() before evaluate() is called for
    // state changes, so if we reach here it has already been cleared.
    if (!is_work_hours())            return AppState::CLOCK;

    // Work hours.
    mock_data::MeetingList list = filtered_meetings();
    if (list.count > 0)              return AppState::MEETING_LIST;
    return AppState::NO_MEETINGS;
}

// ─── Periodic tick ────────────────────────────────────────────────────────

static void tick_cb(lv_timer_t* /*t*/) {
    // 5-minute pre-meeting alert.
    if (g_state != AppState::COUNTDOWN   &&
        g_state != AppState::SETUP       &&
        g_state != AppState::OTA_UPDATING &&
        !is_pto_active()) {

        const char* title = nullptr;
        const char* when  = nullptr;
        int diff_s        = 0;
        int key           = -1;

        if (check_countdown(&title, &when, &diff_s, &key)) {
            g_alerted_meetings.insert(key);
            Serial.printf("[sm] countdown alert for meeting key=%d\n", key);
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
    // Restore mode from NVS.
    g_mode = nvs::get_mode();

    // Create the periodic evaluation timer (1 s cadence).
    g_tick_timer = lv_timer_create(tick_cb, TICK_MS, nullptr);

    Serial.printf("[sm] init: mode=%d first_boot=%d\n",
                  (int)g_mode, (int)nvs::is_first_boot());
}

AppState evaluate() {
    AppState target = compute_target_state();

    // If we're in COUNTDOWN, don't tear it down via evaluate() — the user
    // must tap the Close button to dismiss it (or it times out when the meeting starts).
    if (g_state == AppState::COUNTDOWN && target != AppState::SETUP) {
        return g_state;
    }

    if (target == g_state && g_current_screen != nullptr) {
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
        Serial.printf("[sm] state -> %d  mode=%d\n", (int)g_state, (int)g_mode);
    }

    return g_state;
}

void on_setup_complete() {
    Serial.println("[sm] on_setup_complete");
    nvs::mark_setup_complete();
    g_state = AppState::CLOCK;   // force re-evaluate from non-SETUP
    evaluate();
}

void on_factory_reset() {
    Serial.println("[sm] on_factory_reset");
    // Delegate to the shared factory_reset::execute() so both the local
    // long-press path and the remote BLE path converge here.
    factory_reset::execute();
}

void on_unpair_phone() {
    // Bond wipe wired in M5; for now dismiss the modal and return to runtime.
    Serial.println("[sm] on_unpair_phone");
    g_state = AppState::CLOCK;
    evaluate();
}

void on_mode_toggle() {
    g_mode = (g_mode == 0) ? 1 : 0;
    nvs::set_mode(g_mode);
    Serial.printf("[sm] mode toggle -> %s\n", g_mode ? "Controls" : "Calendar");

    // If switching to Controls but PC is offline, revert immediately.
    if (g_mode == 1 && !g_pc_connected) {
        g_mode = 0;
        nvs::set_mode(0);
        Serial.println("[sm] PC offline — Controls mode reverted to Calendar");
    }

    g_state = AppState::CLOCK;  // force rebuild
    evaluate();
}

void on_ota_begin() {
    Serial.println("[sm] on_ota_begin");
    g_state = AppState::OTA_UPDATING;
    apply_widget_defaults();
    load_screen(build_ota_screen());
}

void on_reconnect_begin() {
    Serial.println("[sm] on_reconnect_begin");
    g_state = AppState::RECONNECT_SYNCING;
    apply_widget_defaults();
    load_screen(build_reconnect_screen());
}

void on_reconnect_end() {
    Serial.println("[sm] on_reconnect_end");
    // Clear the RECONNECT sentinel so compute_target_state() can proceed.
    g_state = AppState::CLOCK;
    evaluate();
}

void set_pc_connected(bool connected) {
    bool changed = (connected != g_pc_connected);
    g_pc_connected = connected;

    if (!connected && g_mode == 1) {
        // Controls mode is useless without Orion — revert to Calendar.
        g_mode = 0;
        nvs::set_mode(0);
        Serial.println("[sm] PC disconnected — Controls mode reverted to Calendar");
    }

    if (changed) {
        g_state = AppState::CLOCK;  // force rebuild
        evaluate();
    }
}

void set_phone_connected(bool connected) {
    g_phone_connected = connected;
    widget_status_bar::set_default_phone_bonded(connected);
    // No screen rebuild needed — the status bar widget handles phone icon
    // visibility independently of the left-panel state machine.
}

AppState current_state() {
    return g_state;
}

uint8_t current_mode() {
    return g_mode;
}

} // namespace state_machine
