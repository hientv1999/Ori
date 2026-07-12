// Custom in-app passkey entry (Windows) — ble-protocol.md §1/§6.1,
// setup-flow.md "Passkey modal (Step 2)". Ori is the *display* side of LE
// Secure Connections Passkey Entry: pairing must be initiated first, and
// only THEN does Ori generate and show its 6-digit code on its own screen.
// Orion is the *entry* side — the user reads that code off Ori and types it
// into our own digit-box modal, arriving here well after the WinRT pairing
// ceremony (and its `PairingRequested` event) has already started.
//
// That ordering is why this can't just take a `passkey: &str` up front: at
// the moment Windows fires `PairingRequested`, the human hasn't typed
// anything yet — the handler has to hold the request open (`GetDeferral`)
// and block until the passkey arrives from the `submit_passkey` command,
// which runs later, driven by the user pressing "Pair" in the UI.
//
// No independent timeout here on purpose: Ori's firmware doesn't implement
// its own passkey-entry deadline either — it inherits NimBLE's spec-mandated
// ~30s Security Manager procedure timer (Bluetooth Core Spec Vol 3 Part H
// §3.4), which fires a real GAP event (`ENC_CHANGE`/`BLE_HS_ETIMEOUT`) that
// Ori's `onAuthenticationComplete()` turns into a disconnect + dismissed
// passkey modal. Orion mirrors that instead of racing it with a second,
// independently-tuned clock: `ble::central::start_pairing` watches the
// peripheral and pushes `PairingInput::Disconnected` through this same
// channel the moment the link actually drops, whatever caused it.
//
// macOS has no equivalent app-level pairing hook (CoreBluetooth exposes
// none) — deferred until that build starts (memory.md).
//
// Blocking, not async: the WinRT objects involved (TypedEventHandler,
// DeviceInformationCustomPairing, ...) wrap raw COM pointers and aren't
// `Send`, so they can't be held across an `.await` inside a future that
// Tauri's command dispatcher requires to be `Send`. `IAsyncOperation::get()`
// blocks the calling thread on a plain Win32 event instead of using Rust's
// async machinery — the caller is expected to run this via
// `tokio::task::spawn_blocking` (see `ble::central::start_pairing`).

use std::sync::mpsc::Receiver;

/// Sent through the same channel from two different places:
/// `ble::central::submit_passkey` (the user typed the code) and the
/// disconnect watcher `start_pairing` spawns (Ori gave up waiting and
/// dropped the link).
pub enum PairingInput {
    Pin(String),
    Disconnected,
}

#[cfg(target_os = "windows")]
pub fn pair_with_passkey_blocking(bluetooth_address: u64, pin_rx: Receiver<PairingInput>) -> Result<(), String> {
    use windows::core::{Error, HSTRING, Ref};
    use windows::Devices::Bluetooth::BluetoothLEDevice;
    use windows::Devices::Enumeration::{
        DeviceInformationCustomPairing, DevicePairingKinds, DevicePairingRequestedEventArgs,
        DevicePairingResultStatus,
    };
    use windows::Foundation::TypedEventHandler;
    use windows::Win32::Foundation::E_ABORT;
    use windows::Win32::System::Com::{CoInitializeEx, COINIT_MULTITHREADED};

    // tokio's blocking-pool threads have no COM apartment by default;
    // WinRT activation expects one. RPC_E_CHANGED_MODE (already
    // initialized, different apartment) and S_FALSE (already initialized,
    // same apartment) are both fine — only a hard failure is fatal.
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
    }

    let device = BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address)
        .map_err(|e| format!("FromBluetoothAddressAsync: {e}"))?
        // windows-future 0.3 renamed the blocking-wait method `IAsyncOperation::get()`
        // to `.join()` (identical semantics — same `Waiter`-based block-until-
        // completed implementation, just a new name); see this crate's `join.rs`.
        .join()
        .map_err(|e| format!("FromBluetoothAddressAsync: {e}"))?;

    let info = device
        .DeviceInformation()
        .map_err(|e| format!("DeviceInformation: {e}"))?;
    let pairing = info.Pairing().map_err(|e| format!("Pairing: {e}"))?;

    // Already bonded from a previous run (e.g. Orion restarted, Ori didn't
    // forget the bond) — nothing to negotiate. The caller's channel sender
    // is simply dropped unused in this case.
    if pairing.IsPaired().unwrap_or(false) {
        return Ok(());
    }

    let custom = pairing.Custom().map_err(|e| format!("Custom: {e}"))?;

    // Fired once Windows has started the pairing ceremony and Ori has (or
    // is about to have) put its passkey on screen. We can't answer yet —
    // take a deferral, block this callback until `submit_passkey` sends the
    // digits the user read off Ori, then accept and complete.
    let handler = TypedEventHandler::<DeviceInformationCustomPairing, DevicePairingRequestedEventArgs>::new(
        move |_sender, args: Ref<DevicePairingRequestedEventArgs>| -> windows::core::Result<()> {
            let args = args.ok()?;
            let deferral = args.GetDeferral()?;
            // Blocks here with no deadline of our own — either the user
            // submits the pin, or the disconnect watcher reports Ori gave
            // up first (see the module comment above). A closed channel
            // with no message (e.g. Orion shutting down mid-pairing) is
            // treated the same as a disconnect.
            let result = match pin_rx.recv() {
                Ok(PairingInput::Pin(pin)) => args.AcceptWithPin(&HSTRING::from(pin.as_str())),
                Ok(PairingInput::Disconnected) | Err(_) => {
                    Err(Error::new(E_ABORT, "Ori disconnected before a passkey was entered"))
                }
            };
            deferral.Complete()?;
            result
        },
    );
    custom
        .PairingRequested(&handler)
        .map_err(|e| format!("PairingRequested: {e}"))?;

    let result = custom
        .PairAsync(DevicePairingKinds::ProvidePin)
        .map_err(|e| format!("PairAsync: {e}"))?
        .join() // windows-future 0.3: `.get()` -> `.join()`, same blocking-wait semantics
        .map_err(|e| format!("PairAsync: {e}"))?;

    let status = result.Status().map_err(|e| format!("Status: {e}"))?;
    if status == DevicePairingResultStatus::Paired || status == DevicePairingResultStatus::AlreadyPaired {
        Ok(())
    } else {
        Err(format!("pairing rejected (status {})", status.0))
    }
}

#[cfg(not(target_os = "windows"))]
pub fn pair_with_passkey_blocking(_bluetooth_address: u64, _pin_rx: Receiver<PairingInput>) -> Result<(), String> {
    // CoreBluetooth exposes no app-level pairing hook — not yet solved for
    // macOS, deferred until that build starts (memory.md).
    Err("custom passkey pairing isn't implemented on this platform yet".into())
}

/// Runs the blocking WinRT pairing call on tokio's blocking thread pool so
/// it never occupies (or blocks) an async worker thread. `pin_rx` is fed by
/// `ble::central::submit_passkey` once the user types Ori's on-screen code.
pub async fn pair_with_passkey(bluetooth_address: u64, pin_rx: Receiver<PairingInput>) -> Result<(), String> {
    tokio::task::spawn_blocking(move || pair_with_passkey_blocking(bluetooth_address, pin_rx))
        .await
        .map_err(|e| format!("pairing task panicked: {e}"))?
}

/// Removes the Windows-level Bluetooth bond (LTK) for `bluetooth_address`.
/// Needed whenever Ori's side of a bond goes away without Windows knowing —
/// a user-initiated Factory Reset (ble-protocol.md §7.2) or a detected
/// factory-reset-during-reconnect (§7.1) — because `pair_with_passkey_blocking`
/// short-circuits on `IsPaired() == true` and skips the passkey ceremony
/// entirely: without this, the next pairing attempt would silently try to
/// reuse an LTK Ori no longer has, and every encrypted write would fail with
/// no passkey prompt ever appearing.
#[cfg(target_os = "windows")]
fn unpair_device_blocking(bluetooth_address: u64) -> Result<(), String> {
    use windows::Devices::Bluetooth::BluetoothLEDevice;
    use windows::Devices::Enumeration::DeviceUnpairingResultStatus;
    use windows::Win32::System::Com::{CoInitializeEx, COINIT_MULTITHREADED};

    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
    }

    let device = BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address)
        .map_err(|e| format!("FromBluetoothAddressAsync: {e}"))?
        .join() // windows-future 0.3: `.get()` -> `.join()`, same blocking-wait semantics
        .map_err(|e| format!("FromBluetoothAddressAsync: {e}"))?;
    let info = device.DeviceInformation().map_err(|e| format!("DeviceInformation: {e}"))?;
    let pairing = info.Pairing().map_err(|e| format!("Pairing: {e}"))?;

    if !pairing.IsPaired().unwrap_or(false) {
        return Ok(()); // Nothing bonded — already in the state we want.
    }

    let result = pairing
        .UnpairAsync()
        .map_err(|e| format!("UnpairAsync: {e}"))?
        .join() // windows-future 0.3: `.get()` -> `.join()`, same blocking-wait semantics
        .map_err(|e| format!("UnpairAsync: {e}"))?;
    let status = result.Status().map_err(|e| format!("Status: {e}"))?;
    if status == DeviceUnpairingResultStatus::Unpaired || status == DeviceUnpairingResultStatus::AlreadyUnpaired {
        Ok(())
    } else {
        Err(format!("unpair rejected (status {})", status.0))
    }
}

#[cfg(not(target_os = "windows"))]
fn unpair_device_blocking(_bluetooth_address: u64) -> Result<(), String> {
    Err("bond removal isn't implemented on this platform yet".into())
}

/// Async wrapper over `unpair_device_blocking` — same `spawn_blocking`
/// treatment as `pair_with_passkey` and for the same reason (non-Send WinRT
/// handles can't cross an `.await` inside a Tauri-required-Send future).
pub async fn unpair_device(bluetooth_address: u64) -> Result<(), String> {
    tokio::task::spawn_blocking(move || unpair_device_blocking(bluetooth_address))
        .await
        .map_err(|e| format!("unpair task panicked: {e}"))?
}

/// Same as `unpair_device_blocking`, but finds the device by its advertised
/// name among devices Windows currently considers paired, instead of needing
/// a Bluetooth address up front. `unpair_device` only ever has an address to
/// work with while Orion is (or very recently was) connected — but Ori's own
/// bond can go stale (factory reset, or the address-type bond-deletion bug
/// fixed on the firmware side) while Orion's `reconnect()` can no longer even
/// get a connection up (e.g. a stale Windows-side bond itself is what's
/// blocking every reconnect attempt at the encrypted-read step, or scanning
/// just never sees the device — ble-protocol.md §7.1's "encryption-failure
/// fallback" needs a live connection to detect the mismatch and never fires
/// if one never forms). Clear All / Factory Reset are the user's only manual
/// escape hatch from that state, so they fall back to this by-name lookup
/// unconditionally — it's a harmless no-op (`Ok(())`) if nothing paired
/// matches the name, e.g. because the connected-path unpair above already
/// handled it.
#[cfg(target_os = "windows")]
fn unpair_device_by_name_blocking(name: &str) -> Result<(), String> {
    use windows::Devices::Bluetooth::BluetoothLEDevice;
    use windows::Devices::Enumeration::{DeviceInformation, DeviceUnpairingResultStatus};
    use windows::Win32::System::Com::{CoInitializeEx, COINIT_MULTITHREADED};

    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
    }

    let selector = BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)
        .map_err(|e| format!("GetDeviceSelectorFromPairingState: {e}"))?;
    let devices = DeviceInformation::FindAllAsyncAqsFilter(&selector)
        .map_err(|e| format!("FindAllAsyncAqsFilter: {e}"))?
        .join() // windows-future 0.3: `.get()` -> `.join()`, same blocking-wait semantics
        .map_err(|e| format!("FindAllAsyncAqsFilter: {e}"))?;

    let count = devices.Size().map_err(|e| e.to_string())?;
    for i in 0..count {
        let info = devices.GetAt(i).map_err(|e| e.to_string())?;
        let dev_name = info.Name().map_err(|e| e.to_string())?.to_string();
        if dev_name != name {
            continue;
        }
        let pairing = info.Pairing().map_err(|e| format!("Pairing: {e}"))?;
        if !pairing.IsPaired().unwrap_or(false) {
            return Ok(());
        }
        let result = pairing
            .UnpairAsync()
            .map_err(|e| format!("UnpairAsync: {e}"))?
            .join() // windows-future 0.3: `.get()` -> `.join()`, same blocking-wait semantics
            .map_err(|e| format!("UnpairAsync: {e}"))?;
        let status = result.Status().map_err(|e| format!("Status: {e}"))?;
        return if status == DeviceUnpairingResultStatus::Unpaired || status == DeviceUnpairingResultStatus::AlreadyUnpaired {
            Ok(())
        } else {
            Err(format!("unpair rejected (status {})", status.0))
        };
    }
    Ok(()) // Nothing currently paired matches this name — nothing to unpair.
}

#[cfg(not(target_os = "windows"))]
fn unpair_device_by_name_blocking(_name: &str) -> Result<(), String> {
    Err("bond removal isn't implemented on this platform yet".into())
}

/// Async wrapper over `unpair_device_by_name_blocking` — same `spawn_blocking`
/// treatment as `unpair_device` and for the same reason.
pub async fn unpair_device_by_name(name: &str) -> Result<(), String> {
    let name = name.to_string();
    tokio::task::spawn_blocking(move || unpair_device_by_name_blocking(&name))
        .await
        .map_err(|e| format!("unpair task panicked: {e}"))?
}
