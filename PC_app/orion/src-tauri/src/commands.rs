// Tauri IPC command surface — the frontend's invoke() targets. Most of
// these are real Phase B logic now: BLE scan/pair/sync (delegating to
// `ble::central`), the connection supervisor that keeps Orion connected to
// Ori across drops and app restarts (`supervise_connection` below), and
// local persistence (`store`). OAuth, calendar source selection, and the
// firmware-update commands are still Phase C/D stubs — canned data or a
// timed event sequence reproducing the prototype's original setTimeout
// choreography, so the already-wired frontend needs no further changes
// when those phases swap the bodies in (ble-protocol.md, ota.md, pc-app.md).

use crate::ble::{ProfileInput, TimeOffInput};
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, WebviewWindow};
use tokio::time::{sleep, Duration};

#[derive(Serialize)]
pub struct InitialState {
    paired: bool,
    connection: &'static str,
    // The frontend's own profile card / editor fields are plain in-memory
    // JS state with no persistence of their own — they're populated once,
    // right after the first-time setup wizard finishes, and never again.
    // On every *subsequent* app launch (a restart, not a fresh pair) nothing
    // ever re-populated them, so the panel showed a blank profile card even
    // though `store::SavedState` on disk (and Ori itself) still had the
    // real data — this field is what the frontend now hydrates itself from
    // on every launch, independent of BLE connection state (profile editing
    // is offline-capable, pc-app.md).
    profile: ProfileInput,
    // Same hazard, same fix, just never extended here originally: Time Off
    // is fully persisted in `store::SavedState` and pushed to Ori, but
    // nothing ever returned it to the frontend on a plain relaunch — the
    // panel showed "no Time Off" even when a real one was still active on
    // both the disk cache and Ori itself, and saving a new one in that state
    // would have silently overwritten the still-valid entry.
    time_off: TimeOffInput,
    // UI language (store::SavedState::language) — same "nothing re-applies
    // this on a plain relaunch" hazard as profile/time_off above, just for
    // app.js's `appLang` instead of a BLE-facing field. Always populated
    // ("en" default rather than `Option`) since app.js's `setAppLang` always
    // wants a concrete code to apply, not an absent-vs-default distinction.
    language: String,
    // iPhone's last-known resolved device type (store::SavedState::
    // phone_device_type) — lets app.js seed lastPhoneBondStatus.d before this
    // session's first connect, same "survives an app restart" reasoning as
    // profile/time_off above, just for the iPhone Info modal instead of the
    // main panel. `None` when never learned or cleared by a reset/unpair.
    phone_device_type: Option<String>,
    // Whether a Calendar Source XML import has already completed in a
    // previous session (store::SavedState::calendar_ics_url) — same
    // "nothing re-applies this on a plain relaunch" hazard as profile/
    // time_off above, just for the Calendar Source settings row's connected/
    // not-connected status instead of a BLE-facing field.
    calendar_connected: bool,
    // Working Hours / Weather Alert / Low Battery Alert (store::SavedState) —
    // same "nothing re-applies this on a plain relaunch" hazard as the
    // fields above, just for the Settings > Alert section's three rows/
    // subscreens instead of a BLE-facing field (none of these are ever
    // pushed to Ori — Working Hours and Weather Alert drive reminders.rs;
    // Low Battery Alert's threshold/enable are read straight into app.js's
    // checkLowBattery()).
    work_hours: crate::store::WorkHours,
    weather_alert: crate::store::WeatherAlert,
    low_battery_alert: crate::store::LowBatteryAlert,
    // Notification Filter's two-level schedule (store::NotificationFilterSchedule,
    // notif_filter.rs) — same "nothing re-applies this on a plain relaunch"
    // hazard as work_hours/weather_alert/low_battery_alert above, just for
    // the main panel's Notification Filter row/subscreen. Orion computes
    // and pushes the actual live BLE value on its own (notif_filter.rs);
    // this is only what seeds the Settings UI.
    notif_filter: crate::store::NotificationFilterSchedule,
    // Bluetooth-radio state (ble/bt_radio.rs), checked once synchronously
    // here so the blocking modal can appear immediately on a cold launch
    // with Bluetooth already off, without waiting for the monitor's first
    // ~3s poll tick — same "avoid a flash of interactive UI" reasoning as
    // `#s-main`'s `pending-init` class.
    bluetooth_available: bool,
}

/// Initial backoff before retrying a failed reconnect, doubling up to
/// `RECONNECT_BACKOFF_MAX` — fast recovery from a brief drop without
/// hammering an Ori that's genuinely powered off or out of range.
const RECONNECT_BACKOFF_INITIAL: Duration = Duration::from_secs(3);
const RECONNECT_BACKOFF_MAX: Duration = Duration::from_secs(30);

/// How many consecutive `ORI_POST_DISCOVERY_FAILURE_PREFIX` failures (device
/// found, but connect/services/sync failed) before treating it as a stale
/// bond rather than a transient blip. Deliberately not 1: a single failure
/// here is well within normal RF-interference noise, and misreading it as
/// "the bond is dead" would force a full re-pair over what was actually a
/// momentary hiccup. 3 consecutive failures, at the backoff schedule above,
/// is at least ~20s of Ori being reachable-but-rejecting-us before Orion
/// gives up on the bond.
const POST_DISCOVERY_FAILURE_THRESHOLD: u32 = 3;

/// Releases the supervisor-running flag on drop, so a panic anywhere inside
/// the supervisor loop can't leave the flag stuck "claimed" — which would
/// silently block every future supervisor from ever starting until an app
/// restart. Belt-and-suspenders: the loop returns normally in every designed
/// path, but the flag is too load-bearing to depend on that.
struct SupervisorGuard(AppHandle);
impl Drop for SupervisorGuard {
    fn drop(&mut self) {
        let state = self.0.state::<crate::ble::BleState>();
        crate::ble::release_supervisor(&state);
    }
}

/// Runs for as long as Orion considers itself paired — keeps it connected
/// to Ori across BLE drops without requiring an app restart (pc-app.md:
/// "the app stays running and synced whether or not the panel is open";
/// ble-protocol.md §11: "background keep-alive"). Started either already
/// connected (right after a first-time pairing completes) or not yet
/// connected (at app launch, from persisted state) — `already_connected`
/// picks which. Only one instance ever runs at a time (`ble::try_claim_supervisor`)
/// — a WebView reload while already paired+connected re-invokes
/// `get_initial_state`, which would otherwise spawn a second loop racing
/// the first.
///
/// Retries an ordinary drop with backoff, forever, until either it
/// reconnects or the user runs Factory Reset / Clear All (which clears
/// `paired` in the store — checked at the top of every loop iteration, so
/// this exits on its own rather than needing an explicit cancel signal).
///
/// Also implements §7.1's stale-bond detection, via two different signals:
/// - A `ble::reconnect` failure carrying `ORI_FACTORY_RESET_PREFIX` means
///   Ori is advertising `SETUP` — its bonds were wiped (the preferred,
///   definitive signal). Acts immediately.
/// - `ORI_POST_DISCOVERY_FAILURE_PREFIX` repeated `POST_DISCOVERY_FAILURE_THRESHOLD`
///   times in a row means Ori is found and reachable but every attempt past
///   discovery fails (the encryption-failure fallback — Windows holding an
///   LTK Ori no longer recognizes). A plain "device not found" resets this
///   counter rather than counting toward it: that just means Ori is
///   currently unreachable (powered off, out of range), which isn't
///   evidence against the bond and must never trigger it — the fallback
///   would otherwise force a full re-pair on every user whose Ori is
///   simply switched off for a while.
///
/// Either path removes the stale Windows-level bond, clears `paired`
/// locally, emits `needs-repair` (the frontend routes this straight back to
/// the setup wizard — see app.js's listener), and stops.
// The claim check used to happen from *inside* this spawned task, racing
// against the caller's own `*state.supervisor_task.lock().await = Some(...)`
// write: a losing instance's AbortHandle could still land in
// `state.supervisor_task` (the caller stores it unconditionally right after
// `tokio::spawn`, before the task has run far enough to know it lost),
// silently orphaning the real, already-running supervisor's tracking — a
// later `abort_supervisor()` would then abort the wrong (already-dead)
// handle while the actual live supervisor keeps running untracked. Moved
// the claim to the caller (see `try_claim_and_spawn_supervisor` below),
// which decides synchronously, before any spawn happens — eliminating the
// race entirely rather than trying to detect it after the fact.
async fn supervise_connection(app: AppHandle, already_connected: bool) {
    let _guard = SupervisorGuard(app.clone());
    supervise_connection_loop(&app, already_connected).await;
}

/// Claims the supervisor slot and spawns `supervise_connection` only if the
/// claim succeeds, storing its `AbortHandle` right there — so
/// `state.supervisor_task` can never be overwritten by a losing attempt.
/// No-op (does not spawn) if another supervisor is already running.
async fn try_claim_and_spawn_supervisor(app: &AppHandle, state: &crate::ble::BleState, already_connected: bool) {
    if !crate::ble::try_claim_supervisor(state) {
        return;
    }
    let task = tokio::spawn(supervise_connection(app.clone(), already_connected));
    *state.supervisor_task.lock().await = Some(task.abort_handle());
}

async fn supervise_connection_loop(app: &AppHandle, already_connected: bool) {
    let mut backoff = RECONNECT_BACKOFF_INITIAL;
    let mut skip_reconnect = already_connected;
    let mut post_discovery_failures = 0u32;
    // `settled_off` = we've shown "off" (Disconnected) once since the last
    // successful connection, so a string of retries settles there and stays
    // quiet rather than flickering. Unlike the old "rec"-on-every-attempt
    // scheme this replaced, there's no separate "have we announced we're
    // trying yet" flag needed here: `ble::reconnect` itself now emits
    // "connecting" the moment it actually finds Ori and "rec" (Syncing) the
    // moment the real sync begins, so the UI only ever moves forward on
    // genuine progress — a scan that never finds anything just leaves the
    // state at "off" (or whatever it last settled to) instead of guessing
    // "rec" up front and hoping discovery succeeds.
    let mut settled_off = false;

    loop {
        let Some(saved) = crate::store::load(app).await else {
            return
        };
        if !saved.paired || saved.device_name.is_empty() {
            return; // Factory Reset / Clear All ran since this loop started.
        }

        if !skip_reconnect {
            let state = app.state::<crate::ble::BleState>();
            // Adopt a link something else already brought up rather than
            // tearing it down with a redundant reconnect — e.g. a re-pair
            // that completed while this supervisor was mid-backoff after a
            // Clear All (the new pairing couldn't claim the supervisor flag,
            // so this old loop is now the live supervisor and must not fight
            // the connection it inherited).
            if !crate::ble::is_connected(&state).await {
                // Bluetooth guard (pc-app.md, `ble/bt_radio.rs`) — don't even
                // attempt a scan/connect while the radio is off. Waiting here
                // (rather than letting `reconnect()` fail its usual way)
                // keeps a Bluetooth-off period from ever reaching the stale-
                // bond detection below (`ORI_POST_DISCOVERY_FAILURE_PREFIX`),
                // which exists for a *reachable-but-rejecting* Ori, not an
                // unusable local radio — and avoids spamming failed attempts
                // the whole time the blocking modal (app.js) is up anyway.
                let mut bt_rx = state.bluetooth_available.subscribe();
                loop {
                    if *bt_rx.borrow() {
                        break;
                    }
                    if bt_rx.changed().await.is_err() {
                        break; // sender dropped — fall through, reconnect() fails normally
                    }
                }

                // Brackets the *entire* attempt — scanning included, not just
                // the found-and-connecting sub-phase `conn-state: "connecting"`
                // covers. Scanning (discover_named_device, inside reconnect())
                // is where most of the wall-clock time goes whenever Ori is
                // genuinely unreachable, and it's silent on `conn-state` (by
                // design — ble-protocol.md's Connecting state only means
                // "found and establishing the link"). Without a signal that
                // spans the scan too, the header's manual reconnect button
                // looked idle/clickable the entire time an attempt was
                // already quietly running, making a click on it a no-op more
                // often than not. This event is what the button's spin +
                // click-guard actually key off (app.js's setReconnectBusy) —
                // decoupled from the header text state on purpose.
                let _ = app.emit("reconnect-attempt", true);
                let result = crate::ble::reconnect(
                    app,
                    &state,
                    &saved.device_name,
                    &saved.profile,
                    &saved.time_off,
                    &saved.shortcuts,
                )
                .await;
                let _ = app.emit("reconnect-attempt", false);
                if let Err(e) = result {
                    if let Some(addr) = e.strip_prefix(crate::ble::ORI_FACTORY_RESET_PREFIX) {
                        let address: u64 = addr.parse().unwrap_or(0);
                        let _ = crate::ble::unpair_bluetooth_bond(address).await;
                        give_up_on_bond(app).await;
                        return;
                    }
                    if let Some(rest) = e.strip_prefix(crate::ble::ORI_POST_DISCOVERY_FAILURE_PREFIX) {
                        post_discovery_failures += 1;
                        if post_discovery_failures >= POST_DISCOVERY_FAILURE_THRESHOLD {
                            if let Some((addr_str, _msg)) = rest.split_once(':') {
                                if let Ok(address) = addr_str.parse::<u64>() {
                                    let _ = crate::ble::unpair_bluetooth_bond(address).await;
                                }
                            }
                            give_up_on_bond(app).await;
                            return;
                        }
                    } else {
                        post_discovery_failures = 0;
                    }
                    // Settle to Disconnected once, then keep retrying quietly.
                    if !settled_off {
                        let _ = app.emit("conn-state", "off");
                        settled_off = true;
                    }
                    // Ensure a clean slate for the next attempt — `reconnect`
                    // can fail with the link still up (e.g. a post-encryption
                    // sync timeout), and the adopt-check above must not mistake
                    // that live-but-unsynced link for a connection to keep.
                    crate::ble::force_disconnect(&state).await;
                    // Races the backoff sleep against a manual nudge from
                    // the header's reconnect button (`commands::force_reconnect`)
                    // — the user just powered Ori back on and doesn't want to
                    // wait out up to 30s of backoff. A forced wake also resets
                    // backoff to the fast cadence, since the user's action is
                    // fresh evidence "worth trying now," not more of the same
                    // failure streak.
                    tokio::select! {
                        _ = sleep(backoff) => {
                            backoff = (backoff * 2).min(RECONNECT_BACKOFF_MAX);
                        }
                        _ = state.reconnect_notify.notified() => {
                            backoff = RECONNECT_BACKOFF_INITIAL;
                        }
                    }
                    continue;
                }
                post_discovery_failures = 0;
            }
        }

        skip_reconnect = false;
        settled_off = false;
        backoff = RECONNECT_BACKOFF_INITIAL;
        let _ = app.emit("conn-state", "on");
        let state = app.state::<crate::ble::BleState>();
        crate::ble::wait_for_disconnect(&state).await;
        let _ = app.emit("conn-state", "off");
        // Loop straight back into a reconnect attempt — no sleep; the
        // device was just here a moment ago.
    }
}

/// Clears local pairing state and tells the frontend to route back to the
/// setup wizard — the shared tail of both §7.1 stale-bond paths above.
///
/// Order matters: `conn-state` MUST be emitted before `needs-repair`, not
/// after. app.js's `setConn()` ends with an unconditional `back()`, which
/// pops whatever's currently on top of the navigation stack — and
/// `needs-repair`'s listener opens the setup wizard (pushing it onto that
/// stack) before this function returns. Emitting `conn-state` last would run
/// that trailing `back()` *after* the wizard is shown, popping it straight
/// back off — the wizard would flash open and instantly vanish, leaving the
/// user stuck on a dead main screen instead of on the wizard.
async fn give_up_on_bond(app: &AppHandle) {
    // Loads its own fresh copy under the store lock rather than taking a
    // `SavedState` handed down from the caller — the supervisor loop's own
    // load (which fed the reconnect attempt this follows) can be many
    // seconds stale by the time a reconnect actually fails, and saving that
    // snapshot back here would silently revert any Settings edit made
    // during that window. Scoped to just this load-mutate-save, released
    // before the identity-cache clear/event emits below.
    {
        let _guard = app.state::<crate::store::StoreLock>().acquire().await;
        let mut saved = crate::store::load(app).await.unwrap_or_default();
        saved.paired = false;
        saved.device_name = String::new();
        // Both callers (SETUP-flag-detected factory reset, and giving up
        // after repeated post-discovery failures) already drop the OS-level
        // Windows bond before reaching here — either way, re-establishing a
        // connection to this device (the same physical unit or a different
        // one entirely) needs a full re-pair ceremony from scratch, which
        // re-learns these fresh. Clearing them here matches `paired`/
        // `device_name` just above: same event (the bond truly ending),
        // same treatment.
        saved.address = None;
        saved.serial_number = None;
        saved.manufacture_date = None;
        // Ori's own bond just ended, which per ble-protocol.md §2 takes its
        // iPhone bond down with it — same event, same treatment as the
        // three fields above.
        saved.phone_device_type = None;
        let _ = crate::store::save(app, saved).await;
    }
    crate::ble::clear_cached_identity(&app.state::<crate::ble::BleState>()).await;
    let _ = app.emit("conn-state", "off");
    let _ = app.emit("needs-repair", ());
}

#[tauri::command]
pub async fn get_initial_state(app: AppHandle) -> InitialState {
    // Fails open (`unwrap_or(true)`) same as the monitor's own poll —
    // an API hiccup on this one synchronous check shouldn't block the whole
    // app from ever showing its main screen.
    let bluetooth_available = crate::ble::bt_radio::bluetooth_radio_on().await.unwrap_or(true);

    let Some(saved) = crate::store::load(&app).await else {
        return InitialState {
            paired: false,
            connection: "off",
            profile: ProfileInput::default(),
            time_off: TimeOffInput::default(),
            language: "en".to_string(),
            phone_device_type: None,
            calendar_connected: false,
            work_hours: crate::store::WorkHours::default(),
            weather_alert: crate::store::WeatherAlert::default(),
            low_battery_alert: crate::store::LowBatteryAlert::default(),
            notif_filter: crate::store::NotificationFilterSchedule::default(),
            bluetooth_available,
        };
    };

    // Favorite combos never travel over BLE (pc-app.md), so unlike
    // profile/time_off/shortcuts they don't need to wait for a connection —
    // seed the session cache now so `run_shortcut` has them ready the
    // moment any connection completes, whether that's this launch's
    // reconnect below or a later (re)pair.
    let state = app.state::<crate::ble::BleState>();
    // Seeds `BleState::bluetooth_available` from the synchronous check above
    // — its `watch::Sender` otherwise defaults to `true` until
    // `bt_radio::spawn_monitor`'s own first poll lands (up to ~3s later,
    // `lib.rs`'s `.setup()`), which would let `try_claim_and_spawn_supervisor`
    // below wave a reconnect attempt through despite Bluetooth already being
    // off at this exact cold-launch moment. A no-op send when already `true`.
    let _ = state.bluetooth_available.send(bluetooth_available);
    crate::ble::set_favorite_combos(&state, saved.combos.clone()).await;
    // Same idea, for device identity (pc-app.md's Ori Info modal) — lets it
    // show the last-known address/serial number/manufacture date, and the
    // iPhone Info modal show the last-known device type, even before this
    // session's first connect completes.
    crate::ble::seed_cached_identity(&state, saved.address.clone(), saved.serial_number.clone(), saved.manufacture_date.clone(), saved.phone_device_type.clone()).await;

    let language = saved.language.clone().unwrap_or_else(|| "en".to_string());
    let calendar_connected = saved.calendar_ics_url.is_some();
    if !saved.paired || saved.device_name.is_empty() {
        return InitialState { paired: false, connection: "off", profile: saved.profile, time_off: saved.time_off, language, phone_device_type: saved.phone_device_type, calendar_connected, work_hours: saved.work_hours, weather_alert: saved.weather_alert, low_battery_alert: saved.low_battery_alert, notif_filter: saved.notif_filter, bluetooth_available };
    }

    // Returns immediately so the UI can render the main screen right away
    // (in the "off"/disconnected state) — the supervisor runs in the
    // background and reports its own results via `conn-state`, which
    // `setConn()` already listens for.
    try_claim_and_spawn_supervisor(&app, &state, false).await;

    InitialState { paired: true, connection: "off", profile: saved.profile, time_off: saved.time_off, language, phone_device_type: saved.phone_device_type, calendar_connected, work_hours: saved.work_hours, weather_alert: saved.weather_alert, low_battery_alert: saved.low_battery_alert, notif_filter: saved.notif_filter, bluetooth_available }
}

/// Working Hours / Weather Alert / Low Battery Alert (pc-app.md's Settings >
/// Alert section, reminders.rs) — all three are plain Orion-local persists,
/// with no pending/dirty-tracking needed for an offline edit (there's no
/// live link to eventually flush any of them to, unlike `save_device_settings`).
/// `save_work_hours` is the one exception to "never pushed to Ori": Working
/// Hours itself is never sent over BLE, but changing it can flip which
/// Notification Filter level should be live right now, so it triggers a
/// best-effort `notif_filter::push_now` after persisting (see below).
#[tauri::command]
pub async fn save_work_hours(app: AppHandle, work_hours: crate::store::WorkHours) -> Result<(), String> {
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.work_hours = work_hours;
    crate::store::save(&app, saved).await?;
    drop(_guard);
    // Shifting the Working Hours window can itself flip which Notification
    // Filter level should be active right now (e.g. moving the end time
    // earlier than the current wall-clock time) — recompute and push
    // immediately rather than waiting for notif_filter's own 60s tick.
    crate::notif_filter::push_now(&app).await;
    Ok(())
}

#[tauri::command]
pub async fn save_weather_alert(app: AppHandle, weather_alert: crate::store::WeatherAlert) -> Result<(), String> {
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.weather_alert = weather_alert;
    crate::store::save(&app, saved).await
}

#[tauri::command]
pub async fn save_low_battery_alert(app: AppHandle, low_battery_alert: crate::store::LowBatteryAlert) -> Result<(), String> {
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.low_battery_alert = low_battery_alert;
    crate::store::save(&app, saved).await
}

/// Auto Do-Not-Disturb schedule (Settings > Notification Filter, pc-app.md/
/// notif_filter.rs) — the two ancs_filter levels Orion switches between on
/// its own, based on Working Hours. Orion-local persistence only, same
/// shape as save_work_hours/save_weather_alert/save_low_battery_alert
/// above; the actual BLE write is a separate concern, pushed immediately
/// after saving so a Settings change while connected takes effect right
/// away rather than waiting for notif_filter's own 60s tick.
#[tauri::command]
pub async fn save_notif_filter_schedule(
    app: AppHandle,
    notif_filter: crate::store::NotificationFilterSchedule,
) -> Result<(), String> {
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.notif_filter = notif_filter;
    crate::store::save(&app, saved).await?;
    drop(_guard);
    crate::notif_filter::push_now(&app).await;
    Ok(())
}

#[tauri::command]
pub fn hide_panel(window: WebviewWindow) {
    let _ = window.hide();
}

/// JS-invokable counterpart to the internal `show_and_focus_panel` call the
/// incoming-call takeover already uses (`ble::central`'s
/// `ancs_call_state_watcher`) — lets app.js's low-battery warning raise the
/// panel the same way even though it isn't driven by a BLE notify handler
/// that already holds an `AppHandle`.
#[tauri::command]
pub fn show_and_focus_window(app: AppHandle) {
    crate::show_and_focus_panel(&app);
}

/// Whether the panel is currently shown (`true`) or hidden — via the tray
/// icon / in-app minimize button, both of which call `hide_panel` above, the
/// only way this panel ever goes away short of quitting. Every "surface this
/// while respecting panel visibility" flow checks this first: visible → show
/// an in-app modal as normal; hidden → fire a native OS notification instead
/// (`show_native_notification` below) rather than force the panel open,
/// respecting the user's choice to keep Orion out of the way. Used by the
/// low-battery warning and the Working Hours rain/snow reminder
/// (`showLowBatteryModal`/`showWorkHoursReminder`, app.js).
#[tauri::command]
pub fn is_panel_visible(window: WebviewWindow) -> bool {
    window.is_visible().unwrap_or(false)
}

/// Native OS toast for any panel-hidden warning (low battery, the Working
/// Hours rain/snow reminder) — Windows draws these bottom-right by default,
/// so no manual positioning is needed. Returns the plugin's error (if any) as
/// a string rather than swallowing it — there's no in-app fallback UI to show
/// instead here (the whole point of this path is that the panel is
/// intentionally hidden), but a silent failure was undiagnosable from
/// outside; app.js logs whatever comes back to the devtools console. Also
/// logged to stderr directly since devtools isn't always open.
///
/// KNOWN WINDOWS LIMITATION (tauri-plugin-notification's own docs): toasts
/// "only work for installed apps" — running via `tauri dev`'s unpackaged
/// binary has no registered AppUserModelID, so Windows shows the toast under
/// a generic host-process name/icon (or, on some Windows 10 builds/configs,
/// declines to render it at all) rather than failing loudly. If this keeps
/// silently no-op'ing even with no error printed, build+install a real
/// bundle (`tauri build`, then run the installed .exe, not the dev one) and
/// re-test there before assuming the trigger logic itself is broken.
#[tauri::command]
pub fn show_native_notification(app: AppHandle, title: String, body: String) -> Result<(), String> {
    use tauri_plugin_notification::NotificationExt;
    app.notification()
        .builder()
        .title(title)
        .body(body)
        .show()
        .map_err(|e| {
            eprintln!("[native notification] failed to show: {e}");
            e.to_string()
        })
}

/// The header's manual reconnect button — visible only while disconnected
/// (app.js's `setConn`). Wakes `supervise_connection_loop` immediately
/// instead of leaving the user to wait out its current exponential backoff:
/// the common case is Ori having been powered off for a while (backoff has
/// climbed toward its 30s ceiling by then) and the user turning it back on
/// and wanting Orion to notice right away. A no-op if there's no supervisor
/// currently sleeping on backoff (e.g. already connected, or an attempt is
/// already in flight) — `Notify::notify_one` just leaves an unused permit.
#[tauri::command]
pub async fn force_reconnect(app: AppHandle) {
    let state = app.state::<crate::ble::BleState>();
    state.reconnect_notify.notify_one();
}

#[tauri::command]
pub async fn ble_scan(app: AppHandle) -> Result<(), String> {
    let state = app.state::<crate::ble::BleState>();
    crate::ble::scan(&app, &state).await
}

#[tauri::command]
pub async fn ble_start_pairing(app: AppHandle, name: String) -> Result<(), String> {
    // Connects and kicks off WinRT's DeviceInformationCustomPairing —
    // that's what actually causes Ori to generate and show its 6-digit
    // code, so it has to happen as soon as the device is picked, before
    // the user has typed anything (setup-flow.md, ble-protocol.md §6.1).
    let state = app.state::<crate::ble::BleState>();
    crate::ble::start_pairing(&app, &state, &name).await
}

/// Backs an in-flight pairing attempt out cleanly — see
/// `ble::cancel_pairing`'s doc comment for why this exists (without it, the
/// passkey modal's Cancel button silently orphaned a parked blocking thread
/// + polling task per cancel-and-retry cycle).
#[tauri::command]
pub async fn ble_cancel_pairing(app: AppHandle) {
    let state = app.state::<crate::ble::BleState>();
    crate::ble::cancel_pairing(&state).await;
}

#[tauri::command]
pub async fn ble_submit_passkey(app: AppHandle, passkey: String, profile: ProfileInput) -> Result<(), String> {
    // Submits the code the user read off Ori's screen (PairingRequested ->
    // ProvidePin) so the OS bonds against it — Orion is the entry side, Ori
    // is the display side. Not yet solved for macOS (CoreBluetooth exposes
    // no equivalent hook) — deferred until that build starts.
    let state = app.state::<crate::ble::BleState>();
    crate::ble::submit_passkey(&app, &state, &passkey, &profile).await?;

    // Paired and synced successfully — remember it so the next launch
    // reconnects instead of dropping back into first-run setup.
    let device_name = state.device_name.lock().await.clone().unwrap_or_default();
    // Load whatever's already on disk and only overwrite the fields this
    // pairing flow actually produced (identity + the profile just submitted)
    // rather than building a fresh `SavedState` with `..Default::default()`
    // for everything else. `time_off`/`shortcuts`/`combos` are Orion-side
    // user preferences, not tied to a specific physical Ori — `give_up_on_bond`
    // (above) deliberately preserves them when a stale bond / on-device
    // factory reset is detected, clearing only `paired`/`device_name` — and
    // the *only* way back from that state is through this same setup wizard,
    // ending here. Building a brand-new default'd `SavedState` on every
    // successful pairing used to silently undo that preservation the moment
    // the user re-paired, discarding a still-valid Time Off entry or
    // shortcut customization for no reason tied to anything the user did.
    // On a genuine first-ever pair, `store::load` returns `None` and
    // `unwrap_or_default()` below reproduces the old all-zeroed behavior
    // exactly, so this changes nothing for that case.
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.paired = true;
    saved.device_name = device_name;
    saved.profile = profile;
    let _ = crate::store::save(&app, saved).await;
    drop(_guard);

    // Hand off to the same supervisor that watches a launch-time reconnect
    // — if this first pairing's link ever drops, Orion should keep trying
    // to get back to Ori on its own rather than only reconnecting on the
    // next app launch. Already connected from the pairing above, so the
    // supervisor's first loop iteration skips straight to watching for a
    // disconnect instead of redundantly reconnecting.
    try_claim_and_spawn_supervisor(&app, &state, true).await;
    Ok(())
}

#[tauri::command]
pub async fn save_profile(app: AppHandle, input: ProfileInput) -> Result<(), String> {
    let state = app.state::<crate::ble::BleState>();

    // Validate a newly-picked photo BEFORE anything below persists it. Ori's
    // `run_sync` re-decodes whatever's cached in `store::SavedState` on
    // *every* subsequent reconnect (not just this one) — a malformed or
    // over-cap photo that slipped past this check would fail that decode
    // forever after, silently wedging every future sync with no
    // user-visible error, since the write below always used to report
    // success regardless. An untouched or removed photo needs no check.
    if !input.photo_removed {
        if let Some(url) = &input.photo_data_url {
            crate::ble::decode_profile_photo(url)?;
        }
    }

    // Persist first, regardless of connection state — the Profile editor is
    // explicitly usable offline (pc-app.md), and a store-persisted edit
    // still reaches Ori on the next reconnect via the hash-manifest delta
    // (§6.2), since it'll hash differently from whatever Ori still has.
    // Pushing first and bailing out on `?` before persisting, as this used
    // to, silently threw the edit away whenever Ori wasn't connected right
    // this moment.
    //
    // Merge into the cached profile rather than overwrite it outright: an
    // untouched photo (photo_data_url absent, photo_removed false) must
    // keep whatever was cached before, or the next reconnect's `run_sync`
    // would hash an empty photo against Ori's real stored one and wipe it
    // out (same hazard `ble::reconnect`'s doc comment describes for the
    // text fields, just for the photo specifically).
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.paired = true;
    if let Some(name) = state.device_name.lock().await.clone() {
        saved.device_name = name;
    }
    saved.profile.name = input.name.clone();
    saved.profile.title = input.title.clone();
    saved.profile.email = input.email.clone();
    saved.profile.phone = input.phone.clone();
    if input.photo_removed {
        saved.profile.photo_data_url = None;
    } else if input.photo_data_url.is_some() {
        saved.profile.photo_data_url = input.photo_data_url.clone();
    }
    // Propagated, not swallowed — a disk-full/permissions failure here used
    // to be silently discarded while the command still reported success, so
    // the UI showed "saved" for an edit that never actually persisted.
    crate::store::save(&app, saved).await?;
    drop(_guard);

    // Best-effort immediate push, scoped to just Profile Info (+ Photo if
    // changed) — ble-protocol.md §6.0/§6.3. A failure here (most commonly
    // "not connected") is not surfaced: the persisted edit above is now the
    // source of truth and will reach Ori on the next reconnect regardless.
    let _ = crate::ble::push_profile(&state, &input).await;
    Ok(())
}

#[tauri::command]
pub async fn save_timeoff(app: AppHandle, input: crate::ble::TimeOffInput) -> Result<(), String> {
    // Validate a newly-picked photo BEFORE anything below persists it — same
    // reasoning as save_profile's identical guard: `run_sync` re-decodes
    // whatever's cached here on every subsequent reconnect, so bad photo
    // data that slipped past this check would silently wedge every future
    // sync rather than just failing this one save.
    if !input.photo_removed {
        if let Some(url) = &input.photo_data_url {
            crate::ble::decode_time_off_photo(url)?;
        }
    }

    // Persist first — see save_profile's comment on why (pc-app.md: Time
    // Off is also editable offline). Same merge-carefully treatment: an
    // untouched photo must keep whatever was cached before, or the next
    // reconnect's run_sync would hash an empty photo against Ori's real
    // stored one and wipe it out (see ble::reconnect's doc comment).
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.time_off.start = input.start;
    saved.time_off.end = input.end;
    saved.time_off.destination = input.destination.clone();
    if input.photo_removed {
        saved.time_off.photo_data_url = None;
    } else if input.photo_data_url.is_some() {
        saved.time_off.photo_data_url = input.photo_data_url.clone();
    }
    // Propagated, not swallowed — see save_profile's comment on why.
    crate::store::save(&app, saved).await?;
    drop(_guard);

    // Best-effort immediate push — a failure here just means Ori gets it on
    // the next reconnect's hash-manifest delta instead.
    let state = app.state::<crate::ble::BleState>();
    let _ = crate::ble::push_time_off(&state, &input).await;
    Ok(())
}

#[tauri::command]
pub async fn clear_timeoff(app: AppHandle) -> Result<(), String> {
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.time_off = Default::default();
    // Propagated, not swallowed — see save_profile's comment on why.
    crate::store::save(&app, saved).await?;
    drop(_guard);

    let state = app.state::<crate::ble::BleState>();
    let _ = crate::ble::clear_time_off(&state).await;
    Ok(())
}

/// Mirrors ble-protocol.md §4's DeviceSettings CBOR map — every field
/// optional, absent keys leave Ori's current state unchanged. Weather
/// (`w`/`d`/`u`) has no caller yet — weather-API integration is Phase D —
/// but the write path itself is real and ready.
/// Shortcut slot tokens ("1"/"2"/"3") aren't here — they ride their own
/// `save_shortcuts` command below, which builds `DeviceSettingsWrite`
/// directly, so this struct only needs the fields this command actually
/// sets. `"f"` (ancs_filter) isn't here either — it's no longer a value the
/// user picks directly; `notif_filter.rs` computes and pushes it on its own
/// schedule, see `save_notif_filter_schedule` below.
#[derive(Deserialize, Default)]
pub struct DeviceSettingsInput {
    c: Option<u8>,
    h: Option<u8>,
    k: Option<u8>,
    w: Option<u8>,
    d: Option<i32>,
    u: Option<u8>,
}

#[tauri::command]
pub async fn save_device_settings(app: AppHandle, settings: DeviceSettingsInput) -> Result<(), String> {
    // Clock face / time format / seek step must survive a save made while
    // Ori isn't reachable (pc-app.md's Disconnected-screen settings) —
    // persist locally first, same reasoning as save_shortcuts. Weather is
    // excluded: it's ephemeral (never meaningfully "pending" — the next
    // live push wins regardless).
    let touches_pending = settings.c.is_some() || settings.h.is_some() || settings.k.is_some();
    if touches_pending {
        // Scoped to just this load-mutate-save — released before the BLE
        // write below, which can take a while and must not serialize every
        // other Settings save behind it.
        let _guard = app.state::<crate::store::StoreLock>().acquire().await;
        let mut saved = crate::store::load(&app).await.unwrap_or_default();
        if let Some(c) = settings.c { saved.pending_clock_face = Some(c); }
        if let Some(h) = settings.h { saved.pending_time_format = Some(h); }
        if let Some(k) = settings.k { saved.pending_seek_step_s = Some(k); }
        crate::store::save(&app, saved).await?;
    }

    let state = app.state::<crate::ble::BleState>();
    let result = crate::ble::set_device_settings(
        &state,
        crate::ble::cbor::DeviceSettingsWrite {
            clock_face: settings.c,
            time_format: settings.h,
            seek_step_s: settings.k,
            weather_condition: settings.w,
            temperature: settings.d,
            temperature_unit: settings.u,
            ..Default::default()
        },
    )
    .await;

    if touches_pending {
        if result.is_ok() {
            // Delivered — clear whichever of these this write covered, but
            // only if the pending value on disk still matches exactly what
            // was just delivered. A concurrent writer (most plausibly
            // run_sync's own pending-flush on a reconnect racing this same
            // Settings Save) could have already changed or cleared it in
            // the meantime — re-loading fresh and checking before clearing
            // avoids silently dropping whichever edit actually lost that
            // race, instead of blindly nulling based on a stale in-memory
            // assumption. run_sync's flush applies the identical check.
            // Fresh guard, not the one from the persist block above (which
            // was already released before the BLE write) — this is its own
            // separate load-mutate-save critical section.
            let _guard = app.state::<crate::store::StoreLock>().acquire().await;
            if let Some(mut saved) = crate::store::load(&app).await {
                if settings.c.is_some() && saved.pending_clock_face == settings.c {
                    saved.pending_clock_face = None;
                }
                if settings.h.is_some() && saved.pending_time_format == settings.h {
                    saved.pending_time_format = None;
                }
                if settings.k.is_some() && saved.pending_seek_step_s == settings.k {
                    saved.pending_seek_step_s = None;
                }
                crate::store::save(&app, saved).await?;
            }
        } else {
            // Not connected (or a transient failure) — the local edit is
            // already safely persisted above and run_sync flushes it on the
            // next successful reconnect. Swallow rather than surface an
            // error the caller would otherwise log/report: from the user's
            // perspective, changing a setting while offline is expected to
            // just work, not fail with a console error.
            return Ok(());
        }
    }
    result
}

#[tauri::command]
pub async fn read_device_settings(app: AppHandle) -> Result<crate::ble::cbor::DeviceSettingsRead, String> {
    // Real read of char 000E on (re)connect (§6.4) — weather is excluded,
    // same as the real device response.
    let state = app.state::<crate::ble::BleState>();
    crate::ble::read_device_settings(&app, &state).await
}

/// Backs the Ori Info/Stats modal — tapping the header's device name +
/// connection state. Read-only, no BLE traffic of its own (see
/// `ble::get_ori_info`'s doc comment on why it just serves what's cached).
#[tauri::command]
pub async fn get_ori_info(app: AppHandle) -> crate::ble::OriInfo {
    let state = app.state::<crate::ble::BleState>();
    crate::ble::get_ori_info(&state).await
}

/// The three Favorite-shortcut key combos, from Orion's local store. Unlike
/// the slot icon tokens (which live on Ori and come back via
/// `read_device_settings`), these are never sent over BLE (pc-app.md —
/// host-side action mapping is Orion-local), so the Quick Actions settings
/// subscreen has to fetch them from here to repopulate its combo display
/// after an app restart. Returns three (possibly empty) lists.
#[tauri::command]
pub async fn get_shortcut_combos(app: AppHandle) -> Vec<Vec<String>> {
    crate::store::load(&app)
        .await
        .map(|s| s.combos.to_vec())
        .unwrap_or_else(|| vec![Vec::new(), Vec::new(), Vec::new()])
}

#[tauri::command]
pub async fn save_shortcuts(app: AppHandle, slots: Vec<String>, combos: Vec<Vec<String>>) -> Result<(), String> {
    let slot1 = slots.first().cloned().unwrap_or_default();
    let slot2 = slots.get(1).cloned().unwrap_or_default();
    let slot3 = slots.get(2).cloned().unwrap_or_default();
    let combo1 = combos.first().cloned().unwrap_or_default();
    let combo2 = combos.get(1).cloned().unwrap_or_default();
    let combo3 = combos.get(2).cloned().unwrap_or_default();

    // Persist first — same reasoning as save_profile/save_timeoff. Doubly
    // true here since shortcuts are pushed unconditionally on every sync
    // regardless of connection state right now (§6.3 "Shortcuts — always"),
    // so a deferred push isn't even a special case, just the normal path.
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.shortcuts = [slot1.clone(), slot2.clone(), slot3.clone()];
    saved.combos = [combo1.clone(), combo2.clone(), combo3.clone()];
    // Propagated, not swallowed — see save_profile's comment on why.
    crate::store::save(&app, saved).await?;
    drop(_guard);

    // Favorite key combos never go over BLE (pc-app.md — host-side action
    // mapping is local to Orion) — just update the session cache
    // `run_shortcut` reads from; always succeeds, no connection needed.
    let state = app.state::<crate::ble::BleState>();
    crate::ble::set_favorite_combos(&state, [combo1, combo2, combo3]).await;

    // Best-effort immediate push of the icon tokens (Device Settings
    // "1"/"2"/"3") — a failure here just means the next (re)connect's
    // unconditional shortcut push delivers it instead.
    let _ = crate::ble::set_device_settings(
        &state,
        crate::ble::cbor::DeviceSettingsWrite {
            slot1: Some(slot1),
            slot2: Some(slot2),
            slot3: Some(slot3),
            ..Default::default()
        },
    )
    .await;
    Ok(())
}

#[tauri::command]
pub async fn unpair_phone(app: AppHandle) -> Result<(), String> {
    let state = app.state::<crate::ble::BleState>();
    let result = crate::ble::unpair_phone(&state).await;
    // Ends the iPhone's bond specifically — Ori's own identity is untouched,
    // so unlike clear_all this only clears the cached device type, not
    // address/serial_number/manufacture_date (store::SavedState::
    // phone_device_type's doc comment).
    crate::ble::clear_cached_phone_device_type(&state).await;
    {
        let _guard = app.state::<crate::store::StoreLock>().acquire().await;
        if let Some(mut saved) = crate::store::load(&app).await {
            saved.phone_device_type = None;
            let _ = crate::store::save(&app, saved).await;
        }
    }
    result
}

/// ANCS drill-down action (Answer/Decline/End call/Dismiss/Read-all) —
/// writes char 0012 (ble-protocol.md §13 "Actions"). `a`: 0=Positive
/// 1=Negative. No optimistic local update — see
/// `ble::ancs_notification_action`'s doc comment for why.
#[tauri::command]
pub async fn ancs_notification_action(app: AppHandle, u: u32, a: u8) -> Result<(), String> {
    let state = app.state::<crate::ble::BleState>();
    crate::ble::ancs_notification_action(&state, u, a).await
}

#[tauri::command]
pub fn set_calendar_source(source: String) {
    let _ = source;
}

/// Caps a field to `max_chars` Unicode scalar values (not bytes) — matches
/// pc-app.md's "name/title/email ≤ 32 chars" UI-enforced limit, which
/// `save_profile`'s frontend caller (`app.js`'s `cc()`) normally guarantees
/// but this command bypasses entirely (its input is a file the user picked,
/// not a form field) — so unlike `save_profile`, this needs its own
/// enforcement rather than trusting the caller.
fn truncate_chars(s: &str, max_chars: usize) -> String {
    s.chars().take(max_chars).collect()
}

/// Result of a successful `import_calendar_xml` — sent back to the frontend
/// so the setup wizard / Calendar Source settings screen can show what was
/// actually extracted and persisted, without a second round-trip read.
#[derive(Serialize)]
pub struct CalendarImportResult {
    pub name: Option<String>,
    pub email: Option<String>,
    pub connected: bool,
    pub meeting_count: usize,
    // Whether the immediate post-import `calendar_import::refresh()` fetch
    // actually succeeded — surfaced separately from `meeting_count` so a
    // network failure (no internet during setup) reads as "couldn't check
    // yet," not silently as "zero meetings today." The XML parse itself
    // (name/email/ical_url extraction) is local and already succeeded by the
    // time this field is set, so `connected` above stays true either way —
    // Next isn't blocked, matching pc-app.md's "warn but let the user
    // proceed" for setup-time network errors.
    pub calendar_fetch_ok: bool,
}

/// Calendar Source — Outlook sharing-invitation XML import (pc-app.md's
/// "Calendar data" section). Deliberately NOT Microsoft Graph/OAuth: Graph
/// requires an Azure AD app registration, which needs tenant-admin consent
/// this product can't assume every user's org will grant (the blocker that
/// killed the originally-built Graph path, `tools/mock_orion_teams`) — the
/// user shares their calendar via Outlook's own Share Calendar → "publish"
/// mechanism instead, which hands back a `sharing_metadata.xml` requiring no
/// app registration, no admin consent, and no OAuth token of any kind.
///
/// Opens a native file picker for that `.xml`, extracts Name/SmtpAddress/
/// ICalUrl (`calendar_import::extract_xml_fields`), merges whatever fields
/// it found into the persisted profile + calendar source (an XML missing a
/// field — e.g. no ICalUrl — leaves that field as whatever was already
/// there, same "don't overwrite with something we don't actually have"
/// merge-carefully treatment `save_profile` uses for an untouched photo),
/// then does one immediate `calendar_import::refresh()` so the caller gets a
/// real `meeting_count` back rather than waiting up to 15 minutes for the
/// next background poll tick.
///
/// Does NOT push the updated profile/meetings to Ori synchronously beyond
/// the best-effort Profile Info write below — Meeting List specifically
/// needs the full hash-manifest BEGIN/END session (`ble-protocol.md` §6.0),
/// which today only runs at initial pairing and at reconnect (no live
/// "resync while already connected" path exists yet for any field). The
/// freshly-cached meetings this call populates are picked up automatically
/// by the next `run_sync` regardless — same as any other pre-connect edit.
#[tauri::command]
pub async fn import_calendar_xml(app: AppHandle) -> Result<CalendarImportResult, String> {
    use tauri_plugin_dialog::DialogExt;

    let picked = app
        .dialog()
        .file()
        .add_filter("Sharing invitation XML", &["xml"])
        .blocking_pick_file();
    let Some(file_path) = picked else {
        return Err("cancelled".into());
    };
    let path = file_path.into_path().map_err(|e| e.to_string())?;
    let xml = std::fs::read_to_string(&path)
        .map_err(|e| format!("couldn't read {}: {e}", path.display()))?;

    let fields = crate::calendar_import::extract_xml_fields(&xml)?;

    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    if let Some(name) = &fields.name {
        saved.profile.name = truncate_chars(name, 32);
    }
    if let Some(email) = &fields.email {
        saved.profile.email = truncate_chars(email, 32);
    }
    if let Some(ical_url) = &fields.ical_url {
        saved.calendar_ics_url = Some(ical_url.clone());
    }

    // Captured before store::save moves `saved` — avoids cloning the whole
    // SavedState (which can carry up to ~950 KB of profile/Time Off photo
    // data, store.rs's own doc comment) just to keep reading these four
    // small string fields and one flag afterward. store::save() takes
    // ownership specifically so callers aren't tempted to clone here.
    let profile_input = ProfileInput {
        name: saved.profile.name.clone(),
        title: saved.profile.title.clone(),
        email: saved.profile.email.clone(),
        phone: saved.profile.phone.clone(),
        photo_data_url: None,
        photo_removed: false,
    };
    let calendar_connected = saved.calendar_ics_url.is_some();

    // Propagated, not swallowed — see save_profile's identical comment on why.
    crate::store::save(&app, saved).await?;
    drop(_guard);

    // Best-effort — a failure (most commonly "not connected yet," true for
    // every first-run-setup call site) just means Ori gets it on the next
    // (re)connect's sync instead, same as save_profile.
    let state = app.state::<crate::ble::BleState>();
    let _ = crate::ble::push_profile(&state, &profile_input).await;

    let (meeting_count, calendar_fetch_ok) = match crate::calendar_import::refresh(&app).await {
        Ok(n) => (n, true),
        Err(_) => (0, false),
    };

    Ok(CalendarImportResult {
        name: Some(profile_input.name).filter(|s| !s.is_empty()),
        email: Some(profile_input.email).filter(|s| !s.is_empty()),
        connected: calendar_connected,
        calendar_fetch_ok,
        meeting_count,
    })
}

/// Reports the frontend's `navigator.geolocation` result (weather.rs) — called
/// once per app launch, independent of pairing state (harmless before Ori is
/// paired; the resolved location is just cached for whenever a connection
/// exists). `lat`/`lon` are `Some` on a successful browser geolocation, or
/// both `None` when the frontend's call was denied, timed out, or
/// `navigator.geolocation` isn't available — weather.rs falls back to
/// IP-based geolocation in that case. See `weather::set_location`'s own doc
/// comment for the full resolution + immediate-fetch flow.
#[tauri::command]
pub async fn set_weather_location(app: AppHandle, lat: Option<f64>, lon: Option<f64>) -> Result<(), String> {
    crate::weather::set_location(&app, lat, lon).await
}

/// User-initiated retry for the header network-warning icon (pc-app.md) —
/// re-runs the exact same `refresh()` calls the calendar/weather background
/// poll loops already run each tick, for when a transient network blip
/// hasn't cleared by itself yet and the user doesn't want to wait out the
/// ~15-min poll interval. Run sequentially, NOT concurrently (`tokio::join!`,
/// as this used to do) — each refresh's own device push (`push_meetings`/
/// `set_device_settings`) takes `BleState::sync_lock` via `try_lock()` and
/// silently no-ops on contention, by design, so a routine background push
/// never blocks/aborts a real bulk sync in progress. That's harmless when
/// the two poll loops' independent ~15/~15-30-min tickers rarely land in
/// the same instant, but joining them here made the collision the COMMON
/// case: after an outage clears, both fetches succeed together and both
/// try to push at once, so whichever lost the `try_lock()` race silently
/// never reached Ori. Both still report through the same `network-health`
/// events the icon already listens for, so this command itself returns
/// nothing — the frontend just awaits it to know when to stop showing its
/// own retry spinner.
#[tauri::command]
pub async fn retry_network_fetch(app: AppHandle) {
    let _ = crate::calendar_import::refresh(&app).await;
    let _ = crate::weather::refresh(&app).await;
}

/// Stops whichever `supervise_connection_loop` is currently running, if any
/// — see `BleState::supervisor_task`'s doc comment for why `clear_all`
/// needs to do this explicitly rather than letting the loop notice the
/// resulting disconnect and report it on its own (that would race the
/// frontend's own reset-triggered navigation to the setup wizard).
async fn abort_supervisor(state: &crate::ble::BleState) {
    let handle = state.supervisor_task.lock().await.take();
    if let Some(handle) = handle {
        handle.abort();
    }
}

/// Falls back to removing Windows' own Bluetooth bond when
/// `ble::factory_reset`'s address-based unpair couldn't run at all (it needs
/// a *live* connection to even learn which peripheral to target, per
/// `ble::factory_reset`'s doc comment) — the exact gap that strands a user
/// whose Ori bond has already gone stale (a prior factory reset, or the
/// address-type bond-deletion bug fixed on the firmware side) while Orion
/// itself can no longer get a connection up at all (a stale Windows-side
/// bond can be *why* every reconnect attempt fails at the encrypted-read
/// step — see `ble::reconnect`'s doc comment on
/// `ORI_POST_DISCOVERY_FAILURE_PREFIX`), and equally the case this was
/// written for: the user hits Reset while Ori is simply disconnected right
/// now (powered off, out of range), not just already-stale. Without this,
/// Clear All / Factory Reset silently no-ops on the one thing that could
/// unstick that loop, and the user is left with no way to re-pair at all.
///
/// Tries two independent lookups — both harmless no-ops if they don't find
/// a match (already handled by the connected path, or never paired):
/// - **By address** (`saved.address`, `store::SavedState`'s write-through
///   cached identity, provisioning.md) — the precise match. `BDAddr`'s
///   `FromStr`/`Into<u64>` round-trip the exact same `Display` format
///   `ble::central::run_sync` wrote it in (`peripheral.address().to_string()`),
///   so this is byte-exact, not a guess.
/// - **By name** (`saved.device_name`) — a second, independent WinRT lookup
///   path (enumerates currently-paired devices and matches the advertised
///   name) kept as a backstop in case the address-based one ever doesn't
///   find the bond Windows still has (e.g. an OS-level enumeration quirk) —
///   cheap insurance, not redundant effort, since the two can't both miss
///   for the same reason.
async fn unpair_stale_windows_bond(saved: &crate::store::SavedState) {
    if let Some(addr_str) = &saved.address {
        if let Ok(bd) = addr_str.parse::<btleplug::api::BDAddr>() {
            let _ = crate::ble::unpair_bluetooth_bond(bd.into()).await;
        }
    }
    if !saved.device_name.is_empty() {
        let _ = crate::ble::unpair_bluetooth_bond_by_name(&saved.device_name).await;
    }
}

#[tauri::command]
pub async fn clear_all(app: AppHandle) {
    // The Reset modal exposes a single consolidated action (memory.md) — this
    // is the only reset command the frontend calls.
    let state = app.state::<crate::ble::BleState>();
    abort_supervisor(&state).await;
    // Held for this whole function, unlike every other command's tightly-
    // scoped guard (which releases before any slow BLE/network step) —
    // Clear All is a rare, deliberate, user-initiated "stop the world"
    // action, and any Settings save that happened to be racing it would
    // just get its data wiped a moment later anyway. Holding the lock the
    // whole way through additionally guarantees no such save can land
    // *after* `store::clear()` below and resurrect the file this was asked
    // to wipe.
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let saved_before = crate::store::load(&app).await;
    let _ = crate::ble::factory_reset(&state).await;
    if let Some(saved) = &saved_before {
        unpair_stale_windows_bond(saved).await;
    }
    crate::ble::reset_session_caches(&state).await;
    crate::store::clear(&app).await;
}

#[derive(Serialize, Clone)]
struct FwProgress {
    pct: f32,
    phase: &'static str,
    version: Option<&'static str>,
    reason: Option<String>,
}

/// Real USB CDC OTA sender (`ota::run_update`) — BEGIN/DATA windowed
/// flow-control/END, driven by real PROGRESS/VALIDATED/FAILED frames from
/// Ori over the serial port (`ota.md`). Interim (Phase C, pre-`orinari.net`
/// hosting): rather than downloading the latest release, prompt for a local
/// `.bin` — defaults to the firmware project's own build output when
/// present, so testing against a freshly-built image needs no typing.
/// Reachable either from the "Ori Update Available" banner (a genuinely
/// newer version detected over BLE, `check_firmware_version`) or the Ori
/// Info modal's "Reinstall Firmware" action (`clickOriInfoReinstall` in
/// app.js) — the latter bypasses the newer-version gate entirely, since
/// same-version reinstall/downgrade is protocol-legal (`ota.md` § "Version
/// & rollback policy") and is how this gets tested without a hosted release
/// to compare against yet.
#[tauri::command]
pub async fn firmware_install(app: AppHandle) {
    use tauri_plugin_dialog::DialogExt;

    let mut builder = app.dialog().file().add_filter("Ori firmware image", &["bin"]);
    let default_dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../firmware/.pio/build/ori");
    if default_dir.is_dir() {
        builder = builder.set_directory(&default_dir);
    }
    let Some(picked) = builder.blocking_pick_file() else {
        let _ = app.emit(
            "fw-progress",
            FwProgress { pct: 0.0, phase: "failed", version: None, reason: Some("No firmware file selected.".to_string()) },
        );
        return;
    };
    let Ok(path) = picked.into_path() else {
        let _ = app.emit(
            "fw-progress",
            FwProgress { pct: 0.0, phase: "failed", version: None, reason: Some("Couldn't read the selected file's path.".to_string()) },
        );
        return;
    };

    let _ = tauri::async_runtime::spawn_blocking(move || crate::ota::run_update(&app, &path)).await;
}

#[tauri::command]
pub async fn orion_update_install(app: AppHandle) {
    let mut pct: f32 = 0.0;
    while pct < 100.0 {
        pct = (pct + 4.0).min(100.0);
        let phase = if pct < 65.0 { "downloading" } else { "installing" };
        let _ = app.emit("orion-update-progress", FwProgress { pct, phase, version: None, reason: None });
        sleep(Duration::from_millis(60)).await;
    }
    let _ = app.emit(
        "orion-update-progress",
        FwProgress { pct: 100.0, phase: "ready", version: None, reason: None },
    );
}

#[tauri::command]
pub fn orion_restart(app: AppHandle) {
    // Phase D: app.restart() once the self-update actually swaps the binary.
    let _ = app;
}

/// Backs the Settings panel's "Start at login" toggle (`pc-app.md`'s
/// "Option to start at user login (`tauri-plugin-autostart`)") — reads the
/// OS's actual registered-startup-entry state rather than a value Orion
/// cached, so the toggle reflects reality even if the entry was removed or
/// added outside the app (e.g. Windows' own Startup Apps settings, or Task
/// Manager's "Disable"). `tauri_plugin_autostart::ManagerExt::autolaunch()`
/// hands back the plugin-managed `AutoLaunchManager` state.
#[tauri::command]
pub async fn get_autostart_enabled(app: AppHandle) -> Result<bool, String> {
    use tauri_plugin_autostart::ManagerExt;
    app.autolaunch().is_enabled().map_err(|e| e.to_string())
}

/// Enables/disables Orion's OS-level "run at login" entry via
/// `tauri-plugin-autostart` — the write side of `get_autostart_enabled`.
#[tauri::command]
pub async fn set_autostart_enabled(app: AppHandle, enabled: bool) -> Result<(), String> {
    use tauri_plugin_autostart::ManagerExt;
    let manager = app.autolaunch();
    if enabled { manager.enable() } else { manager.disable() }.map_err(|e| e.to_string())
}

/// Persists the UI language (`store::SavedState::language`) — the write side
/// of `get_initial_state`'s `language` field. Called by app.js's
/// `setAppLang` on every language-selector change (welcome screen and
/// Settings both use the same selector/handler). Unlike the
/// clock-face/time-format/ANCS-filter settings, this has no device-side
/// counterpart to reconcile with — it's purely local to Orion, so there's no
/// pending/offline-edit tracking needed, just a plain load-mutate-save.
#[tauri::command]
pub async fn set_language(app: AppHandle, code: String) -> Result<(), String> {
    let _guard = app.state::<crate::store::StoreLock>().acquire().await;
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.language = Some(code);
    crate::store::save(&app, saved).await
}
