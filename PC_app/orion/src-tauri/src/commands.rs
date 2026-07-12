// Phase A stubs — every command here returns canned data or emits a timed
// sequence of events that reproduces the prototype's original setTimeout
// choreography, so the frontend (already wired for real invoke/listen) needs
// no further changes when Phase B swaps these bodies for real BLE/OTA/OAuth
// logic (ble-protocol.md, ota.md, pc-app.md).

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, WebviewWindow};
use tokio::time::{sleep, Duration};

#[derive(Serialize)]
pub struct InitialState {
    paired: bool,
    connection: &'static str,
}

#[tauri::command]
pub fn get_initial_state() -> InitialState {
    // Phase B: read the cached bond + last-known connection state instead of
    // always starting from a clean slate.
    InitialState {
        paired: false,
        connection: "off",
    }
}

#[tauri::command]
pub fn hide_panel(window: WebviewWindow) {
    let _ = window.hide();
}

#[derive(Serialize, Clone)]
struct ScanResult {
    name: &'static str,
    strong: bool,
}

#[tauri::command]
pub async fn ble_scan(app: AppHandle) {
    // Phase B: real btleplug scan, filtered to the Ori Sync Service UUID
    // (ble-protocol.md §2/§3), emitting as devices are discovered rather
    // than as one batch after a fixed delay.
    sleep(Duration::from_millis(1200)).await;
    let devices = vec![
        ScanResult { name: "Ori-XT-9F", strong: true },
        ScanResult { name: "Ori-4C-12", strong: false },
    ];
    let _ = app.emit("scan-result", devices);
}

#[allow(dead_code)]
#[derive(Deserialize)]
pub struct ProfileInput {
    name: String,
    title: String,
    email: String,
    phone: String,
}

#[derive(Serialize, Clone)]
struct SyncProgress {
    pct: f32,
    label: Option<&'static str>,
    done: bool,
}

#[tauri::command]
pub async fn ble_pair(app: AppHandle, name: String, profile: ProfileInput) {
    let _ = (&name, &profile); // Phase B: carries into the real BEGIN/END sync (ble-protocol.md §6.1)

    // Phase B: this delay is real OS pairing-dialog + LE Secure Connections
    // bonding time, not a sleep — see setup-flow.md's "Confirm pairing" step.
    sleep(Duration::from_millis(1500)).await;
    let _ = app.emit("pairing-connecting", ());

    sleep(Duration::from_millis(400)).await;
    let mut pct: f32 = 0.0;
    while pct < 100.0 {
        pct = (pct + 8.0).min(100.0);
        let _ = app.emit(
            "sync-progress",
            SyncProgress { pct, label: None, done: false },
        );
        sleep(Duration::from_millis(90)).await;
    }
    let _ = app.emit(
        "sync-progress",
        SyncProgress { pct: 100.0, label: None, done: true },
    );
}

#[allow(dead_code)]
#[derive(Deserialize)]
pub struct SaveProfileInput {
    name: String,
    title: String,
    email: String,
    phone: String,
    #[serde(rename = "photoDataUrl")]
    photo_data_url: Option<String>,
    #[serde(rename = "photoRemoved")]
    photo_removed: bool,
}

#[tauri::command]
pub fn save_profile(input: SaveProfileInput) {
    // Phase B: resize photo to 228x228 JPEG (<=200KB), hash-check, push via
    // Profile Info / Profile Photo characteristics (ble-protocol.md §4/§10).
    let _ = input;
}

#[allow(dead_code)]
#[derive(Deserialize)]
pub struct SaveTimeOffInput {
    start: i64,
    end: i64,
    destination: String,
    #[serde(rename = "photoDataUrl")]
    photo_data_url: Option<String>,
    #[serde(rename = "photoRemoved")]
    photo_removed: bool,
}

#[tauri::command]
pub fn save_timeoff(input: SaveTimeOffInput) {
    // Phase B: resize photo to 528x396 JPEG (<=512KB), push via Time Off Entry.
    let _ = input;
}

#[tauri::command]
pub fn clear_timeoff() {}

/// Mirrors ble-protocol.md §4's DeviceSettings CBOR map — every field
/// optional, absent keys leave Ori's current state unchanged.
#[allow(dead_code)]
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
}

#[tauri::command]
pub fn save_device_settings(settings: DeviceSettingsInput) {
    // Phase B: write to char 000E outside the BEGIN/END pipeline (§6.4).
    let _ = settings;
}

#[derive(Serialize)]
pub struct DeviceSettingsState {
    c: u8,
    h: u8,
    f: u8,
    #[serde(rename = "1")]
    slot1: String,
    #[serde(rename = "2")]
    slot2: String,
    #[serde(rename = "3")]
    slot3: String,
}

#[tauri::command]
pub fn read_device_settings() -> DeviceSettingsState {
    // Phase B: real read of char 000E on (re)connect (§6.4) — presence and
    // weather are excluded, same as the real device response.
    DeviceSettingsState {
        c: 0,
        h: 0,
        f: 3,
        slot1: "vol-mute".into(),
        slot2: "mic-mute".into(),
        slot3: "screenshot".into(),
    }
}

#[tauri::command]
pub fn save_shortcuts(slots: Vec<String>, combos: Vec<Vec<String>>) {
    // Phase B: icon tokens -> Device Settings "1"/"2"/"3"; Favorite combos
    // stay local to Orion (pc-app.md — host-side action mapping is local).
    let _ = (slots, combos);
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

#[tauri::command]
pub fn factory_reset() {
    // Phase B: write Device Command magic 0xFA C7 5E 5E over the bonded link.
}

#[tauri::command]
pub fn clear_all() {
    // Phase B: factory_reset() + wipe Orion's local cache (profile, calendar
    // sign-in, shortcuts).
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
