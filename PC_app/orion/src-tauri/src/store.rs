// Minimal local persistence — just enough for Orion to remember it's
// already paired across an app restart, instead of always dropping back
// into first-run setup. The real BLE bond (LTK) already survives a
// restart on its own, at the OS level (Windows' own Bluetooth stack) —
// this file is Orion's own memory of *that fact*, plus the profile fields
// needed to answer a reconnect's hash-manifest honestly (see the comment
// on `crate::ble::reconnect` for why an empty/blank profile on reconnect
// would be actively harmful, not just a missing nice-to-have) — and the
// same reasoning applies to the shortcut slot tokens, just outside the
// hash-manifest mechanism (`run_sync` sends them unconditionally on every
// sync, ble-protocol.md §6.3, so a blank/default cache here would silently
// reset the user's chosen icons on every reconnect).
//
// Deliberately small otherwise: no calendar sign-in — that's a separate,
// later piece (pc-app.md's "Clear All" describes a broader local cache this
// will eventually grow into). Bond address/serial number/manufacture date
// (below) were added once the Ori Info modal needed to show them without a
// live connection — see those fields' own doc comment.

use crate::ble::{ProfileInput, TimeOffInput};
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Manager};

fn default_shortcuts() -> [String; 3] {
    ["vol-mute".into(), "mic-mute".into(), "screenshot".into()]
}

#[derive(Serialize, Deserialize, Clone)]
pub struct SavedState {
    pub paired: bool,
    pub device_name: String,
    pub profile: ProfileInput,
    // Same hazard as `profile` — a reconnect that hashed a blank Time Off
    // against Ori's real stored entry would get it overwritten with empty
    // fields. Defaults to the "nothing set" zeroed entry, which is honest
    // until the user actually sets one via Settings.
    pub time_off: TimeOffInput,
    // Firmware-default combo until the user changes a slot in Settings —
    // matches `BleState::shortcut_slots`'s own default so a first pair and
    // an "always had the default" reconnect behave identically.
    #[serde(default = "default_shortcuts")]
    pub shortcuts: [String; 3],
    // Favorite shortcut key combos (per slot) — never sent over BLE
    // (pc-app.md: host-side action mapping is local), so unlike `shortcuts`
    // there's no wire hazard in leaving this empty; it's persisted purely so
    // a recorded combo still fires after an app restart, not just within the
    // session that recorded it.
    #[serde(default)]
    pub combos: [Vec<String>; 3],
    // Clock face / time format / notification filter changed while Ori
    // wasn't reachable (pc-app.md's Disconnected-screen settings) — `None`
    // means no unsynced local edit for that field (defer to whatever Ori
    // itself reports on next read). Unlike `shortcuts` above, these three
    // are normally push-ON-CHANGE-only (ble-protocol.md §6.3), not pushed
    // unconditionally on every reconnect — so without this, an offline edit
    // would have no path back to Ori at all once the app moved on from the
    // settings screen. Flushed and cleared by `run_sync`'s first successful
    // write after reconnecting; survives an app restart in the meantime
    // since it's just an ordinary persisted field like the rest of this
    // struct.
    #[serde(default)]
    pub pending_clock_face: Option<u8>,
    #[serde(default)]
    pub pending_time_format: Option<u8>,
    #[serde(default)]
    pub pending_ancs_filter: Option<u8>,
    // Device identity — Bluetooth address, serial number, manufacture date
    // (provisioning.md). Unlike everything else in this struct, these never
    // change for a given bond, so they're written through to disk the first
    // time each is learned (ble::central's `run_sync`/`read_device_settings`)
    // and read back on every app launch — this is what lets the Ori Info
    // modal (pc-app.md) show them even before a connection completes this
    // session, not just "cached within a session" like firmware_version.
    // Cleared only when the bond itself ends: `commands::give_up_on_bond`
    // (Ori was factory reset elsewhere) explicitly nulls these three out;
    // `clear_all` wipes the whole struct via `store::clear()` anyway.
    #[serde(default)]
    pub address: Option<String>,
    #[serde(default)]
    pub serial_number: Option<String>,
    #[serde(default)]
    pub manufacture_date: Option<String>,
    // iPhone's resolved marketing model name (e.g. "iPhone 17 Pro Max",
    // ble-protocol.md's PhoneBondStatus "d") — same "never changes for a
    // given bond" reasoning as serial_number/manufacture_date above, so it
    // gets the same write-through-to-disk treatment (central.rs's
    // `phone_bond_watcher`) rather than living only in the session's
    // `lastPhoneBondStatus` cache, which used to get blanked on every Orion
    // disconnect even though the iPhone itself hadn't gone anywhere. Cleared
    // alongside address/serial_number/manufacture_date on Factory Reset /
    // Clear All / `give_up_on_bond` (Ori's OWN bond ending also drops its
    // iPhone bond, ble-protocol.md §2) — and additionally on a standalone
    // `unpair_phone` (commands.rs), since that ends the iPhone's bond without
    // touching Ori's own identity fields.
    #[serde(default)]
    pub phone_device_type: Option<String>,
    // UI language ("en"/"vi"/"es"/"fr", app.js's I18N keys) — an Orion app
    // preference, not anything about the Ori pairing relationship, so unlike
    // `address`/`serial_number`/`manufacture_date` above it is NOT cleared by
    // Factory Reset or the passive "Ori was reset elsewhere" path
    // (`commands::give_up_on_bond`) — only `clear_all`'s full-file wipe
    // resets it, same as a genuinely fresh install (which then falls back to
    // `None` → app.js's own "en" default). `None` here also covers every
    // `state.json` written before this field existed (`#[serde(default)]`).
    #[serde(default)]
    pub language: Option<String>,
}

impl Default for SavedState {
    fn default() -> Self {
        Self {
            paired: false,
            device_name: String::new(),
            profile: ProfileInput::default(),
            time_off: TimeOffInput::default(),
            shortcuts: default_shortcuts(),
            combos: [Vec::new(), Vec::new(), Vec::new()],
            pending_clock_face: None,
            pending_time_format: None,
            pending_ancs_filter: None,
            address: None,
            serial_number: None,
            manufacture_date: None,
            phone_device_type: None,
            language: None,
        }
    }
}

fn state_file_path(app: &AppHandle) -> Result<std::path::PathBuf, String> {
    let dir = app.path().app_data_dir().map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir.join("state.json"))
}

fn load_blocking(app: &AppHandle) -> Option<SavedState> {
    let path = state_file_path(app).ok()?;
    let bytes = std::fs::read(path).ok()?;
    serde_json::from_slice(&bytes).ok()
}

fn save_blocking(app: &AppHandle, state: &SavedState) -> Result<(), String> {
    let path = state_file_path(app)?;
    let bytes = serde_json::to_vec_pretty(state).map_err(|e| e.to_string())?;
    std::fs::write(path, bytes).map_err(|e| e.to_string())
}

fn clear_blocking(app: &AppHandle) {
    if let Ok(path) = state_file_path(app) {
        let _ = std::fs::remove_file(path);
    }
}

// `load`/`save`/`clear` run the actual `std::fs` work on tokio's blocking
// thread pool (`spawn_blocking`) rather than directly on an async worker
// thread — same idiom already used for the WinRT pairing calls
// (`ble::pairing::pair_with_passkey`) and JPEG re-encoding
// (`ble::central::push_album_art`). These are called from async Tauri
// command handlers (`get_initial_state`, `ble_submit_passkey`,
// `save_profile`, `save_timeoff`, `clear_timeoff`, `save_shortcuts`,
// `clear_all`) and, more importantly, from inside
// `supervise_connection_loop`'s retry loop — a synchronous filesystem call
// there would block that worker thread on every single reconnect/backoff
// iteration, for as long as Orion keeps retrying (which can be indefinitely,
// while Ori is powered off).

pub async fn load(app: &AppHandle) -> Option<SavedState> {
    let app = app.clone();
    tokio::task::spawn_blocking(move || load_blocking(&app)).await.ok().flatten()
}

// Takes `state` by value rather than `&SavedState` — every call site already
// holds its own owned `SavedState` it never reuses afterward (built fresh via
// `store::load` + a few field mutations right before calling this), so a
// by-reference signature only forced a redundant clone here on top of that.
// `SavedState` can embed up to ~950KB of profile/Time-Off photo base64
// (ble-protocol.md §10's caps), so that clone was a real, avoidable copy on
// every single save — not just a one-field settings change.
pub async fn save(app: &AppHandle, state: SavedState) -> Result<(), String> {
    let app = app.clone();
    tokio::task::spawn_blocking(move || save_blocking(&app, &state))
        .await
        .map_err(|e| format!("store save task panicked: {e}"))?
}

/// Wipes the persisted state — factory reset / Clear All.
pub async fn clear(app: &AppHandle) {
    let app = app.clone();
    let _ = tokio::task::spawn_blocking(move || clear_blocking(&app)).await;
}
