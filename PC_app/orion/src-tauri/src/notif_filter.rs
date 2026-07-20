// Auto Do-Not-Disturb (pc-app.md) — Orion, not the user, decides which of
// the two configured `store::NotificationFilterSchedule` levels is active
// on Ori's `ancs_filter` (Device Settings "f") at any given moment, based on
// `store::WorkHours`. Ori itself has no concept of a schedule — it just
// always shows whatever single filter level it was last told, exactly as
// before this feature existed; only Orion's decision of WHAT to write, and
// WHEN, is new.

use tauri::{AppHandle, Manager};
use tokio::sync::Mutex;

/// Tracks the effective filter value most recently computed (and best-
/// effort pushed) this session — same "always update regardless of push
/// success" reasoning as `weather.rs`'s own `last_pushed`: a transient BLE
/// failure just means the next reconnect's `push_last_known` resends this
/// same value. Reset implicitly on every recompute, not on disconnect —
/// there's nothing to invalidate, since the value only ever depends on the
/// schedule/work-hours settings and the current time, not on connection state.
#[derive(Default)]
pub struct NotifFilterState {
    last_computed: Mutex<Option<u8>>,
}

/// True when `minutes_of_day` falls inside the configured working window.
/// Handles `start`/`end` in either order:
///   - `start < end`:  a normal same-day window (e.g. 8:00-16:30) — in the
///     window iff today is a configured working day AND `start <= now < end`.
///   - `start > end`:  an overnight-spanning window (e.g. 22:00-6:00). Split
///     into its two calendar-day halves, each gated on the day THAT HALF
///     actually belongs to: `now >= start` is tonight's half (gated on
///     today's day flag); `now < end` is this morning's tail of the shift
///     that started YESTERDAY evening (gated on YESTERDAY's day flag, NOT
///     today's — gating both halves on today's flag would wrongly cut the
///     early-morning tail short whenever yesterday and today differ, e.g. a
///     Friday-22:00-to-Saturday-06:00 shift with only Friday enabled must
///     still read as "in the window" at Saturday 03:00).
///   - `start == end`: a zero-width window. Treated as "never in work
///     hours" rather than "24 hours" — a single instant matches nothing,
///     and there's no UI signal for "all day" to justify guessing the other
///     way. (See the Working Hours edge-case question this feature grew out
///     of — this is the one place that degenerate input now has real
///     behavior riding on it.)
fn in_work_window(wh: &crate::store::WorkHours, minutes_of_day: u16, weekday: u8) -> bool {
    let (start, end) = (wh.start_minutes, wh.end_minutes);
    match start.cmp(&end) {
        std::cmp::Ordering::Equal => false,
        std::cmp::Ordering::Less => wh.days[weekday as usize] && minutes_of_day >= start && minutes_of_day < end,
        std::cmp::Ordering::Greater => {
            let yesterday = (weekday + 6) % 7; // Mon=0..Sun=6, one day back with wraparound
            (wh.days[weekday as usize] && minutes_of_day >= start)
                || (wh.days[yesterday as usize] && minutes_of_day < end)
        }
    }
}

fn effective_filter(schedule: &crate::store::NotificationFilterSchedule, wh: &crate::store::WorkHours) -> u8 {
    let (minutes_of_day, weekday) = crate::ble::central::local_now_minutes_and_weekday();
    if in_work_window(wh, minutes_of_day, weekday) {
        schedule.work_filter
    } else {
        schedule.off_filter
    }
}

async fn write_filter(ble_state: &crate::ble::BleState, value: u8) -> Result<(), String> {
    crate::ble::set_device_settings(
        ble_state,
        crate::ble::cbor::DeviceSettingsWrite { ancs_filter: Some(value), ..Default::default() },
    )
    .await
}

/// Recomputes the effective filter and pushes it to Ori only when it
/// differs from what was last computed (`force=false`), or unconditionally
/// (`force=true`, used right after a Settings save so the change feels
/// immediate rather than waiting for the next tick). Best-effort: a failed
/// write (most commonly "not connected right now") is swallowed the same
/// way `weather.rs`'s poll loop does — `last_computed` is updated either
/// way, and the next reconnect's `push_last_known` sends the correct value.
async fn apply(app: &AppHandle, saved: &crate::store::SavedState, force: bool) {
    let value = effective_filter(&saved.notif_filter, &saved.work_hours);

    {
        let state = app.state::<NotifFilterState>();
        let mut last = state.last_computed.lock().await;
        if !force && *last == Some(value) {
            return;
        }
        *last = Some(value);
    }

    let ble_state = app.state::<crate::ble::BleState>();
    let _ = write_filter(&ble_state, value).await;
}

/// Called right after `save_notif_filter_schedule`/`save_work_hours`
/// persist a Settings change, so it takes effect immediately while
/// connected instead of waiting up to a minute for the next merged tick.
/// This is a one-off call outside the periodic loop, so — unlike `tick`
/// below — it loads its own copy of the store rather than sharing one.
pub async fn push_now(app: &AppHandle) {
    let saved = crate::store::load(app).await.unwrap_or_default();
    apply(app, &saved, true).await;
}

/// Takes an already-loaded `saved` rather than calling `store::load` itself —
/// see `reminders::tick`'s identical doc comment for why (`lib.rs`'s merged
/// 60s tick loads the store once and shares it with both). This is what
/// catches a work/off-hours boundary crossing (or a Settings edit made from
/// another device/session) while Orion stays connected straight through it,
/// since nothing else would otherwise trigger a re-push between reconnects.
pub async fn tick(app: &AppHandle, saved: &crate::store::SavedState) {
    apply(app, saved, false).await;
}

/// Re-sends whatever the schedule currently resolves to, unconditionally —
/// called from `start_post_sync_tasks` on every (re)connect, same reasoning
/// as `weather::push_last_known`/`holiday::push_last_known`: Ori's actual
/// `ancs_filter` may be stale (the schedule, Working Hours, or just the
/// wall clock crossing a boundary, all could have changed while
/// disconnected), and re-sending an already-correct value is a harmless
/// no-op.
pub async fn push_last_known(app: &AppHandle, ble_state: &crate::ble::BleState) -> Result<(), String> {
    let saved = crate::store::load(app).await.unwrap_or_default();
    let value = effective_filter(&saved.notif_filter, &saved.work_hours);
    *app.state::<NotifFilterState>().last_computed.lock().await = Some(value);
    write_filter(ble_state, value).await
}

