// Bluetooth-guard (pc-app.md) — real "is the Bluetooth radio switched on"
// detection, distinct from btleplug's own adapter-found/not-found signal:
// toggling Bluetooth off in Windows Settings still leaves the adapter
// hardware enumerable (`central.rs`'s `get_adapter()` would keep finding it),
// so only `Windows.Devices.Radios`' own on/off state is reliable here. Used
// to show a non-dismissable blocking modal and pause the BLE reconnect loop
// (`commands.rs`'s `supervise_connection_loop`) while Bluetooth is off —
// calendar/weather polling keep running regardless, per pc-app.md.

use tauri::{AppHandle, Emitter, Manager};

/// `Ok(true)` iff a Bluetooth radio is present and its `RadioState` is `On`.
/// `Ok(false)` when a radio is present but off/disabled, or no Bluetooth
/// radio exists on this PC at all — both read the same to callers here ("no
/// usable Bluetooth right now").
#[cfg(target_os = "windows")]
pub async fn bluetooth_radio_on() -> Result<bool, String> {
    use windows::Devices::Radios::{Radio, RadioKind, RadioState};

    let radios = Radio::GetRadiosAsync()
        .map_err(|e| e.to_string())?
        .await
        .map_err(|e| e.to_string())?;
    let count = radios.Size().map_err(|e| e.to_string())?;
    for i in 0..count {
        let radio = radios.GetAt(i).map_err(|e| e.to_string())?;
        if radio.Kind().map_err(|e| e.to_string())? == RadioKind::Bluetooth {
            return Ok(radio.State().map_err(|e| e.to_string())? == RadioState::On);
        }
    }
    Ok(false) // no Bluetooth radio present at all
}

/// macOS: deferred, not designed — same status as the custom passkey-entry
/// pairing flow (`pairing.rs`, `memory.md`'s "Orion pairing UX"). Never
/// blocks: reporting "always on" here means the Bluetooth guard is simply
/// inert on macOS until that build starts, rather than falsely gating a
/// platform this hasn't been built for yet.
#[cfg(not(target_os = "windows"))]
pub async fn bluetooth_radio_on() -> Result<bool, String> {
    Ok(true)
}

const POLL_INTERVAL: std::time::Duration = std::time::Duration::from_secs(3);

/// Spawned once from `lib.rs`'s `.setup()`. Fails open (`unwrap_or(true)`) if
/// the API call itself errors — an API hiccup shouldn't block the whole app,
/// same reasoning `app.js`'s `showLowBatteryModal` already applies to
/// `is_panel_visible`. On an actual state change: updates
/// `BleState::bluetooth_available` (read by `supervise_connection_loop` to
/// pause/resume BLE reconnects), emits `bluetooth-state` for the frontend's
/// blocking modal, and — only when it just went OFF — brings the panel to
/// the foreground so the modal is actually seen (same forced-foreground
/// treatment as the low-battery/incoming-call paths).
pub fn spawn_monitor(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        let mut last: Option<bool> = None;
        loop {
            let on = bluetooth_radio_on().await.unwrap_or(true);
            if last != Some(on) {
                last = Some(on);
                let state = app.state::<crate::ble::BleState>();
                let _ = state.bluetooth_available.send(on);
                let _ = app.emit("bluetooth-state", on);
                if !on {
                    crate::show_and_focus_panel(&app);
                }
            }
            tokio::time::sleep(POLL_INTERVAL).await;
        }
    });
}
