// Weather Alert end-of-day rain/snow reminder (pc-app.md) — entirely an
// Orion-local feature, no BLE involved: neither `store::WorkHours` (start/end
// time + day-of-week picker) nor `store::WeatherAlert` (enable + offset,
// Settings) is ever pushed to Ori. `WeatherAlert.offset_minutes` minutes
// before `WorkHours.end_minutes`, if the most recently polled weather
// condition (weather.rs's ~15-min poll) is Rain/Thunderstorm/Snow, this
// fires a `work-hours-reminder` event the same way `commands.rs`'s
// low-battery path does — app.js decides in-app modal vs native toast off
// `is_panel_visible`.

use tauri::{AppHandle, Emitter, Manager};

/// Ori's `DeviceSettings.weather_condition` values that this reminder cares
/// about (weather.rs's own `Condition` enum, kept private to that module —
/// duplicated here as raw bytes rather than exported, since this is the only
/// other file that needs to name them).
const CONDITION_RAIN: u8 = 3;
const CONDITION_THUNDERSTORM: u8 = 4;
const CONDITION_SNOW: u8 = 5;

/// Tracks the local epoch-day (days since 1970-01-01, local time) this
/// reminder last actually FIRED on — set only once rain/snow is confirmed
/// and the reminder is about to emit, never on a merely-checked-but-clear
/// tick (see `tick`'s own comment on why that distinction matters). A day
/// change (or an app restart) naturally re-arms it with no explicit reset
/// logic.
#[derive(Default)]
pub struct RemindersState {
    last_checked_day: tokio::sync::Mutex<Option<i64>>,
}

/// One check: is right now within the configured minutes-before-end-of-day
/// window, on a configured working day, with rain/snow in the forecast, and
/// is Weather Alert enabled? Fires at most once per local calendar day.
///
/// Takes an already-loaded `saved` rather than calling `store::load` itself
/// — `lib.rs`'s merged 60s tick loads it once and passes the same copy to
/// both this and `notif_filter::tick`, instead of each independently
/// re-reading `state.json` (a `spawn_blocking` file read that can carry up
/// to ~950 KB of profile/Time Off photo data, per `store.rs`'s own doc
/// comment) every minute.
///
/// The window is `[trigger_minute, end_minutes]`, not a single exact-minute
/// match against `trigger_minute` — this poll only ticks every 60s, and a
/// delayed tick (system sleep/wake, a slow prior tick) could skip straight
/// past one specific minute, silently missing the reminder for the rest of
/// the day with no catch-up. Widening to a range bounded by `end_minutes`
/// gives a tick that lands late a chance to still catch it, while never
/// firing once the workday it's about (leaving work soon) has actually
/// ended.
///
/// `RemindersState::last_checked_day` is set only once rain/snow is
/// actually confirmed, at the bottom of this function — NOT on first entry
/// into the window. Weather updates roughly every 15 min (weather.rs's own
/// poll), so a tick landing early in the window under clear conditions must
/// not claim the whole day: doing so used to mean a forecast that turned
/// rainy/snowy a few minutes later, still inside the same window, was
/// silently never reminded about. Checking the condition first and marking
/// the day second means every tick in the window keeps trying until either
/// it fires (marked, capped at once per day) or the window closes.
pub async fn tick(app: &AppHandle, saved: &crate::store::SavedState) {
    let wa = saved.weather_alert.clone();
    if !wa.enabled {
        return;
    }
    let wh = saved.work_hours.clone();

    let (minutes_of_day, weekday) = crate::ble::central::local_now_minutes_and_weekday();
    if !wh.days[weekday as usize] {
        return;
    }

    let trigger_minute = wh.end_minutes.saturating_sub(wa.offset_minutes);
    if minutes_of_day < trigger_minute || minutes_of_day > wh.end_minutes {
        return;
    }

    let weather_state = app.state::<crate::weather::WeatherState>();
    let Some(condition) = crate::weather::last_condition(&weather_state).await else { return };
    let kind = match condition {
        CONDITION_RAIN => "rain",
        CONDITION_THUNDERSTORM => "thunderstorm",
        CONDITION_SNOW => "snow",
        _ => return,
    };

    let today = crate::ble::central::local_epoch_day();
    let state = app.state::<RemindersState>();
    {
        let mut last_checked_day = state.last_checked_day.lock().await;
        if *last_checked_day == Some(today) {
            return; // already fired today
        }
        *last_checked_day = Some(today);
    }

    let _ = app.emit(
        "work-hours-reminder",
        serde_json::json!({"kind": kind, "end_minutes": wh.end_minutes}),
    );
}

