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
    eprintln!("[ORION-DEBUG] supervise_connection: entered (already_connected={already_connected})");
    let _guard = SupervisorGuard(app.clone());
    supervise_connection_loop(&app, already_connected).await;
    eprintln!("[ORION-DEBUG] supervise_connection_loop: returned, guard about to drop (releases supervisor_running)");
}

/// Claims the supervisor slot and spawns `supervise_connection` only if the
/// claim succeeds, storing its `AbortHandle` right there — so
/// `state.supervisor_task` can never be overwritten by a losing attempt.
/// No-op (does not spawn) if another supervisor is already running.
async fn try_claim_and_spawn_supervisor(app: &AppHandle, state: &crate::ble::BleState, already_connected: bool) {
    if !crate::ble::try_claim_supervisor(state) {
        eprintln!("[ORION-DEBUG] try_claim_and_spawn_supervisor: another supervisor is already running — not spawning");
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
            eprintln!("[ORION-DEBUG] supervisor loop: no saved state — exiting");
            return
        };
        if !saved.paired || saved.device_name.is_empty() {
            eprintln!("[ORION-DEBUG] supervisor loop: paired={} device_name={:?} — exiting", saved.paired, saved.device_name);
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
                eprintln!("[ORION-DEBUG] supervisor loop: calling reconnect() for {:?}", saved.device_name);
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
                eprintln!("[ORION-DEBUG] supervisor loop: reconnect() returned {:?}", result);
                if let Err(e) = result {
                    if let Some(addr) = e.strip_prefix(crate::ble::ORI_FACTORY_RESET_PREFIX) {
                        let address: u64 = addr.parse().unwrap_or(0);
                        let _ = crate::ble::unpair_bluetooth_bond(address).await;
                        give_up_on_bond(app, saved).await;
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
                            give_up_on_bond(app, saved).await;
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
async fn give_up_on_bond(app: &AppHandle, mut saved: crate::store::SavedState) {
    saved.paired = false;
    saved.device_name = String::new();
    // Both callers (SETUP-flag-detected factory reset, and giving up after
    // repeated post-discovery failures) already drop the OS-level Windows
    // bond before reaching here — either way, re-establishing a connection
    // to this device (the same physical unit or a different one entirely)
    // needs a full re-pair ceremony from scratch, which re-learns these
    // fresh. Clearing them here matches `paired`/`device_name` just above:
    // same event (the bond truly ending), same treatment.
    saved.address = None;
    saved.serial_number = None;
    saved.manufacture_date = None;
    let _ = crate::store::save(app, &saved).await;
    crate::ble::clear_cached_identity(&app.state::<crate::ble::BleState>()).await;
    let _ = app.emit("conn-state", "off");
    let _ = app.emit("needs-repair", ());
}

#[tauri::command]
pub async fn get_initial_state(app: AppHandle) -> InitialState {
    let Some(saved) = crate::store::load(&app).await else {
        return InitialState {
            paired: false,
            connection: "off",
            profile: ProfileInput::default(),
            time_off: TimeOffInput::default(),
            language: "en".to_string(),
        };
    };

    // Favorite combos never travel over BLE (pc-app.md), so unlike
    // profile/time_off/shortcuts they don't need to wait for a connection —
    // seed the session cache now so `run_shortcut` has them ready the
    // moment any connection completes, whether that's this launch's
    // reconnect below or a later (re)pair.
    let state = app.state::<crate::ble::BleState>();
    crate::ble::set_favorite_combos(&state, saved.combos.clone()).await;
    // Same idea, for device identity (pc-app.md's Ori Info modal) — lets it
    // show the last-known address/serial number/manufacture date even
    // before this session's first connect completes.
    crate::ble::seed_cached_identity(&state, saved.address.clone(), saved.serial_number.clone(), saved.manufacture_date.clone()).await;

    let language = saved.language.clone().unwrap_or_else(|| "en".to_string());
    if !saved.paired || saved.device_name.is_empty() {
        return InitialState { paired: false, connection: "off", profile: saved.profile, time_off: saved.time_off, language };
    }

    // Returns immediately so the UI can render the main screen right away
    // (in the "off"/disconnected state) — the supervisor runs in the
    // background and reports its own results via `conn-state`, which
    // `setConn()` already listens for.
    eprintln!("[ORION-DEBUG] get_initial_state: trying to claim+spawn supervisor (already_connected=false)");
    try_claim_and_spawn_supervisor(&app, &state, false).await;

    InitialState { paired: true, connection: "off", profile: saved.profile, time_off: saved.time_off, language }
}

#[tauri::command]
pub fn hide_panel(window: WebviewWindow) {
    let _ = window.hide();
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
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.paired = true;
    saved.device_name = device_name;
    saved.profile = profile;
    let _ = crate::store::save(&app, &saved).await;

    // Hand off to the same supervisor that watches a launch-time reconnect
    // — if this first pairing's link ever drops, Orion should keep trying
    // to get back to Ori on its own rather than only reconnecting on the
    // next app launch. Already connected from the pairing above, so the
    // supervisor's first loop iteration skips straight to watching for a
    // disconnect instead of redundantly reconnecting.
    eprintln!("[ORION-DEBUG] ble_submit_passkey: trying to claim+spawn supervisor (already_connected=true)");
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
    crate::store::save(&app, &saved).await?;

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
    crate::store::save(&app, &saved).await?;

    // Best-effort immediate push — a failure here just means Ori gets it on
    // the next reconnect's hash-manifest delta instead.
    let state = app.state::<crate::ble::BleState>();
    let _ = crate::ble::push_time_off(&state, &input).await;
    Ok(())
}

#[tauri::command]
pub async fn clear_timeoff(app: AppHandle) -> Result<(), String> {
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.time_off = Default::default();
    // Propagated, not swallowed — see save_profile's comment on why.
    crate::store::save(&app, &saved).await?;

    let state = app.state::<crate::ble::BleState>();
    let _ = crate::ble::clear_time_off(&state).await;
    Ok(())
}

/// Mirrors ble-protocol.md §4's DeviceSettings CBOR map — every field
/// optional, absent keys leave Ori's current state unchanged. Presence
/// (`p`) and weather (`w`/`d`/`u`) have no caller yet — Teams/weather-API
/// integration is Phase D — but the write path itself is real and ready.
#[derive(Deserialize, Default)]
pub struct DeviceSettingsInput {
    p: Option<u8>,
    #[serde(rename = "1")]
    slot1: Option<String>,
    #[serde(rename = "2")]
    slot2: Option<String>,
    #[serde(rename = "3")]
    slot3: Option<String>,
    c: Option<u8>,
    h: Option<u8>,
    f: Option<u8>,
    w: Option<u8>,
    d: Option<i32>,
    u: Option<u8>,
}

#[tauri::command]
pub async fn save_device_settings(app: AppHandle, settings: DeviceSettingsInput) -> Result<(), String> {
    // Clock face / time format / notification filter must survive a save
    // made while Ori isn't reachable (pc-app.md's Disconnected-screen
    // settings) — persist locally first, same reasoning as save_shortcuts.
    // Presence/weather/slots are excluded: presence and weather are
    // ephemeral (never meaningfully "pending" — the next live push wins
    // regardless), and slots already have their own always-on-reconnect
    // push (run_sync) so they need no dirty-tracking of their own.
    let touches_pending = settings.c.is_some() || settings.h.is_some() || settings.f.is_some();
    if touches_pending {
        let mut saved = crate::store::load(&app).await.unwrap_or_default();
        if let Some(c) = settings.c { saved.pending_clock_face = Some(c); }
        if let Some(h) = settings.h { saved.pending_time_format = Some(h); }
        if let Some(f) = settings.f { saved.pending_ancs_filter = Some(f); }
        crate::store::save(&app, &saved).await?;
    }

    let state = app.state::<crate::ble::BleState>();
    let result = crate::ble::set_device_settings(
        &state,
        crate::ble::cbor::DeviceSettingsWrite {
            presence: settings.p,
            slot1: settings.slot1,
            slot2: settings.slot2,
            slot3: settings.slot3,
            clock_face: settings.c,
            time_format: settings.h,
            ancs_filter: settings.f,
            weather_condition: settings.w,
            temperature: settings.d,
            temperature_unit: settings.u,
        },
    )
    .await;

    if touches_pending {
        if result.is_ok() {
            // Delivered — clear whichever of the three this write covered so
            // run_sync doesn't redundantly re-push an already-synced value
            // on the next reconnect.
            let mut saved = crate::store::load(&app).await.unwrap_or_default();
            if settings.c.is_some() { saved.pending_clock_face = None; }
            if settings.h.is_some() { saved.pending_time_format = None; }
            if settings.f.is_some() { saved.pending_ancs_filter = None; }
            crate::store::save(&app, &saved).await?;
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
    // Real read of char 000E on (re)connect (§6.4) — presence and weather
    // are excluded, same as the real device response.
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
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.shortcuts = [slot1.clone(), slot2.clone(), slot3.clone()];
    saved.combos = [combo1.clone(), combo2.clone(), combo3.clone()];
    // Propagated, not swallowed — see save_profile's comment on why.
    crate::store::save(&app, &saved).await?;

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
    crate::ble::unpair_phone(&state).await
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

#[tauri::command]
pub async fn oauth_google() {
    // Phase D: system-browser OAuth + loopback redirect, token in Keychain/
    // Credential Locker.
    sleep(Duration::from_millis(1400)).await;
}

#[tauri::command]
pub async fn oauth_microsoft() {
    sleep(Duration::from_millis(1400)).await;
}

#[tauri::command]
pub fn oauth_signout(provider: String) {
    let _ = provider;
}

/// Stops whichever `supervise_connection_loop` is currently running, if any
/// — see `BleState::supervisor_task`'s doc comment for why
/// `factory_reset`/`clear_all` need to do this explicitly rather than
/// letting the loop notice the resulting disconnect and report it on its
/// own (that would race the frontend's own reset-triggered navigation to
/// the setup wizard).
async fn abort_supervisor(state: &crate::ble::BleState) {
    let handle = state.supervisor_task.lock().await.take();
    eprintln!("[ORION-DEBUG] abort_supervisor: had a tracked handle = {}", handle.is_some());
    if let Some(handle) = handle {
        handle.abort();
    }
}

/// Falls back to removing Windows' own Bluetooth bond by name when
/// `ble::factory_reset`'s address-based unpair couldn't run (it needs a live
/// connection, `ble::factory_reset`'s doc comment) — the exact gap that
/// strands a user whose Ori bond has already gone stale (a prior factory
/// reset, or the address-type bond-deletion bug fixed on the firmware side)
/// while Orion itself can no longer get a connection up at all (a stale
/// Windows-side bond can be *why* every reconnect attempt fails at the
/// encrypted-read step — see `ble::reconnect`'s doc comment on
/// `ORI_POST_DISCOVERY_FAILURE_PREFIX`). Without this, Clear All / Factory
/// Reset silently no-op on the one thing that could unstick that loop, and
/// the user is left with no way to re-pair at all. Harmless to call
/// unconditionally: a no-op if nothing paired matches `device_name` (already
/// handled by the connected path, or never paired).
async fn unpair_stale_windows_bond(device_name: &str) {
    if device_name.is_empty() {
        return;
    }
    let _ = crate::ble::unpair_bluetooth_bond_by_name(device_name).await;
}

#[tauri::command]
pub async fn factory_reset(app: AppHandle) {
    eprintln!("[ORION-DEBUG] factory_reset command invoked");
    let state = app.state::<crate::ble::BleState>();
    abort_supervisor(&state).await;
    let saved_before = crate::store::load(&app).await;
    let _ = crate::ble::factory_reset(&state).await;
    if let Some(saved) = &saved_before {
        unpair_stale_windows_bond(&saved.device_name).await;
    }

    // The just-reset device will re-advertise as a fresh unit; wipe the
    // per-session caches so a re-pair in this same session pushes firmware
    // defaults, not the previous owner's shortcut tokens (see
    // ble::reset_session_caches).
    crate::ble::reset_session_caches(&state).await;

    // Drop the local bond record regardless of whether the write above
    // succeeded (Ori may have already started rebooting before an ack
    // arrived — see ble::factory_reset's doc comment), so Orion doesn't
    // try to reconnect to a device that just wiped its own bond on next
    // launch. pc-app.md: "Orion's local profile cache is untouched," so
    // only `paired`/`device_name`/identity are cleared, not the cached
    // profile. Identity (address/serial_number/manufacture_date) IS cleared
    // here, unlike profile: it's specific to the physical unit that was just
    // wiped, not authored data Orion re-pushes on the next pair regardless
    // (provisioning.md — the values themselves survive on Ori's own factory
    // partition, but Orion has no way to confirm this device is still the
    // same one out from under a fresh pairing ceremony without asking again).
    if let Some(mut saved) = saved_before {
        saved.paired = false;
        saved.device_name = String::new();
        saved.address = None;
        saved.serial_number = None;
        saved.manufacture_date = None;
        let _ = crate::store::save(&app, &saved).await;
    }
}

#[tauri::command]
pub async fn clear_all(app: AppHandle) {
    eprintln!("[ORION-DEBUG] clear_all command invoked");
    // This is the button the UI actually calls (the Reset modal was
    // consolidated to a single action, memory.md) — factory_reset above is
    // otherwise unreachable from the frontend right now, but kept as its
    // own command for anything that wants "reset the device only."
    let state = app.state::<crate::ble::BleState>();
    abort_supervisor(&state).await;
    let saved_before = crate::store::load(&app).await;
    let _ = crate::ble::factory_reset(&state).await;
    if let Some(saved) = &saved_before {
        unpair_stale_windows_bond(&saved.device_name).await;
    }
    crate::ble::reset_session_caches(&state).await;
    crate::store::clear(&app).await;
}

#[derive(Serialize, Clone)]
struct FwProgress {
    pct: f32,
    phase: &'static str,
    version: Option<&'static str>,
}

#[tauri::command]
pub async fn firmware_install(app: AppHandle) {
    // Phase C: real USB CDC OTA sender (ota.md) — BEGIN/DATA windowed
    // flow-control/END, driven by real PROGRESS frames from Ori.
    let mut pct: f32 = 0.0;
    while pct < 100.0 {
        pct = (pct + 3.0).min(100.0);
        let phase = if pct < 55.0 {
            "downloading"
        } else if pct < 80.0 {
            "verifying"
        } else {
            "installing"
        };
        let _ = app.emit("fw-progress", FwProgress { pct, phase, version: None });
        sleep(Duration::from_millis(60)).await;
    }
    let _ = app.emit(
        "fw-progress",
        FwProgress { pct: 100.0, phase: "done", version: Some("1.1.0") },
    );
}

#[tauri::command]
pub async fn orion_update_install(app: AppHandle) {
    let mut pct: f32 = 0.0;
    while pct < 100.0 {
        pct = (pct + 4.0).min(100.0);
        let phase = if pct < 65.0 { "downloading" } else { "installing" };
        let _ = app.emit("orion-update-progress", FwProgress { pct, phase, version: None });
        sleep(Duration::from_millis(60)).await;
    }
    let _ = app.emit(
        "orion-update-progress",
        FwProgress { pct: 100.0, phase: "ready", version: None },
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
    let mut saved = crate::store::load(&app).await.unwrap_or_default();
    saved.language = Some(code);
    crate::store::save(&app, &saved).await
}

/// TEMPORARY DEBUG: lets the frontend forward a log line to the same stderr
/// stream as the backend's `[ORION-DEBUG]` output, so a single terminal
/// capture shows both the Rust BLE path and the JS ANCS-store path in one
/// interleaved log. Remove once the ANCS-list delivery issue is resolved.
#[tauri::command]
pub fn debug_log(msg: String) {
    eprintln!("{msg}");
}
