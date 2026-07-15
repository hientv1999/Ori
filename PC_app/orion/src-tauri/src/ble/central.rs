// BLE central role — scanning, connecting, the custom passkey pairing
// handoff, the hash-manifest sync flow, Device Settings read/write, the
// Controls-mode OS bridge, and the connection supervisor that keeps Orion
// connected to Ori across drops and app restarts (ble-protocol.md
// §2/§6/§7.1/§11/§12; the supervisor loop itself lives in commands.rs).
//
// Scan/connect/pair/sync: a first-sync and a reconnect-delta sync are
// unified into one flow (mirroring tools/mock_orion_ble.py's run_sync() —
// see the comment above `run_sync` below for why that's correct, not just
// convenient).
//
// Presence and weather push have a real, working write path
// (`set_device_settings`/`write_device_settings`) but no caller yet — Teams
// presence and a weather-API poll are Phase D (pc-app.md), which hasn't
// started. Ori's own documented fallback (Offline / hidden weather) is the
// honest state in the meantime (ble-protocol.md §6.4).

use crate::ble::{cbor, chunk, gatt, media, pairing};
use btleplug::api::{
    Central, CentralEvent, Characteristic, ConnectionParameterPreset, Manager as _, Peripheral as _, ScanFilter,
    ValueNotification, WriteType,
};
use btleplug::platform::{Adapter, Manager, Peripheral, PeripheralId};
use futures::StreamExt;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::pin::Pin;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager as _};
use tokio::sync::Mutex;
use uuid::Uuid;

pub struct BleState {
    adapter: Mutex<Option<Adapter>>,
    peripheral: Mutex<Option<Peripheral>>,
    // Set by `start_pairing` while the WinRT `PairingRequested` handler is
    // blocked waiting for the user to type the passkey Ori is showing on
    // its own screen. Fed from two places: `submit_passkey` sends the typed
    // digits; the disconnect watcher spawned alongside it sends
    // `Disconnected` if Ori gives up first (its own NimBLE pairing-procedure
    // timeout) — see `ble::pairing`'s module comment for why Orion doesn't
    // run an independent timer here.
    pending_pin_tx: Mutex<Option<std::sync::mpsc::Sender<pairing::PairingInput>>>,
    // The spawned WinRT pairing operation — `submit_passkey` awaits this
    // (after feeding the pin above) to learn whether pairing succeeded.
    pairing_task: Mutex<Option<tokio::task::JoinHandle<Result<(), String>>>>,
    // GATT service discovery runs concurrently with pairing (not before it —
    // that would delay Ori putting its passkey on screen) so it's normally
    // already done by the time `submit_passkey` needs `run_sync`'s
    // characteristics; `submit_passkey` still awaits it to be sure.
    discover_task: Mutex<Option<tokio::task::JoinHandle<Result<(), String>>>>,
    // In-memory record of the shortcut tokens last written to Ori's slots
    // 1/2/3 (Device Settings, §12) — there's no local settings store yet
    // (a separate, later piece), so this is session memory only: it
    // doesn't survive an Orion restart, but it's what `shortcut` dispatch
    // in the media bridge needs to know which OS action a given slot maps
    // to right now. Defaults match the combo `run_sync` writes on connect.
    shortcut_slots: Mutex<[String; 3]>,
    // Favorite shortcut key combos never travel over BLE — the host-side
    // action mapping is local to Orion (pc-app.md) — so this is the local
    // counterpart to `shortcut_slots`: session memory `run_shortcut`'s
    // "favorite" dispatch reads from, seeded from `store::SavedState.combos`
    // at startup (`commands::get_initial_state`) and updated whenever the
    // user records a new combo (`commands::save_shortcuts`).
    favorite_combos: Mutex<[Vec<String>; 3]>,
    // Serializes every BEGIN/END mid-session push (`run_sync`, `push_profile`,
    // `push_time_off`, the periodic Time Sync refresh) against each other —
    // the staging model (ble-protocol.md §6.0) assumes one session open at a
    // time; without this, e.g. a periodic Time Sync tick landing while the
    // user clicks Save Profile could interleave two BEGIN/END sequences
    // sharing the same seq on the wire.
    sync_lock: Mutex<()>,
    // Every per-connection background task spawned by `start_post_sync_tasks`
    // — media/volume bridge, phone bond watcher, firmware-version check,
    // periodic Time Sync refresh. Each is `abort()`ed at the top of
    // `start_post_sync_tasks` before its replacement is spawned: even though
    // they self-exit event-driven now (via `disconnect_tx`, `watch_for_
    // disconnect`'s doc comment), a fast back-to-back reconnect can still
    // abort THAT watcher (itself respawned per-connection) before it relays
    // the old connection's disconnect — so a task racing that exact window
    // would otherwise never learn its own connection ended, and could run
    // alongside its replacement, e.g. dispatching every KeyboardCommand
    // twice. The explicit abort here is what actually closes that race,
    // not just a nice-to-have on top of the event.
    background_tasks: Mutex<Vec<tokio::task::JoinHandle<()>>>,
    // The two ANCS relay watcher tasks (char 0010/0011 drain loops), owned
    // and lifecycle-managed by `subscribe_ancs_relay_early` — see its doc
    // comment for why they can't be folded into `background_tasks` above
    // and spawned later by `start_post_sync_tasks` the way the other four
    // per-connection tasks are.
    ancs_tasks: Mutex<Vec<tokio::task::JoinHandle<()>>>,
    // Set by `start_pairing`/`reconnect` — read back by the
    // `ble_submit_passkey` command on success so it knows what to write
    // into `store::SavedState` (the command itself only receives the
    // passkey and profile, not the device name).
    pub device_name: Mutex<Option<String>>,
    // Guards against more than one `supervise_connection` loop running at
    // once — a WebView reload re-invokes `get_initial_state`, which would
    // otherwise spawn a second supervisor racing the first (see
    // `commands::supervise_connection`).
    supervisor_running: std::sync::atomic::AtomicBool,
    // Bumped by `start_pairing` (to the new in-flight attempt's id) and by
    // `cancel_pairing` (to invalidate whatever attempt was running). The
    // spawned pairing task captures the id it was started with and compares
    // against this before emitting `pairing-failed`, so a cancelled attempt
    // that later fails on its own (unblocked by `cancel_pairing`'s
    // `Disconnected` send) doesn't pop a stale failure dialog over whatever
    // the user is doing by the time it resolves.
    pairing_generation: std::sync::atomic::AtomicU64,
    // Lets `commands::force_reconnect` (the header's manual reconnect
    // button) wake `supervise_connection_loop` immediately instead of
    // leaving it to sleep out its current exponential backoff (up to 30s) —
    // e.g. the user powered Ori back on after a while and doesn't want to
    // wait for backoff to catch up. A no-op if nothing's currently waiting
    // (no supervisor running, or one that's mid-attempt rather than
    // sleeping) — `notify_one` just leaves a permit that's silently unused
    // in that case, never a problem.
    pub reconnect_notify: tokio::sync::Notify,
    // Event-driven disconnect signal, bumped by `watch_for_disconnect` the
    // instant btleplug relays a genuine link-layer disconnect for the
    // current peripheral — subscribed to by `wait_for_disconnect` and every
    // per-connection watcher in this file instead of polling `is_connected()`
    // on a timer. A `watch` channel specifically because each subscriber
    // independently tracks whether it's seen the latest value, so a
    // disconnect can never be missed the way it could between one fixed-
    // cadence poll and the next.
    disconnect_tx: tokio::sync::watch::Sender<u64>,
    // The task currently relaying disconnects into `disconnect_tx` for the
    // live connection — aborted and replaced on every (re)connect, same
    // reasoning as `ancs_tasks`/`background_tasks` below.
    disconnect_watcher: Mutex<Option<tokio::task::JoinHandle<()>>>,
    // Handle to whichever `supervise_connection_loop` task is currently
    // running (set at both of its spawn sites — `get_initial_state` and
    // `ble_submit_passkey`). `commands::factory_reset`/`clear_all` abort it
    // directly rather than letting it notice the resulting disconnect on
    // its own: that loop's normal tail (`wait_for_disconnect` → emit
    // `conn-state:"off"`) is correct for an *unexpected* drop, but firing it
    // after a user-initiated reset races the frontend's own
    // `setConn('off'); openSetupWizard();` sequence — `setConn`'s trailing
    // `back()` would pop the wizard the reset just opened. There's nothing
    // left for the loop to supervise after a reset anyway (no
    // `store::SavedState` to reconnect with), so aborting it immediately is
    // just doing on the spot what it would've done on its own next
    // iteration — minus the stray emit.
    pub supervisor_task: Mutex<Option<tokio::task::AbortHandle>>,
    // Backing the Ori Info/Stats modal (pc-app.md's panel — device identity
    // snapshot). `cached_fw_version`/`last_synced` are genuinely session-only
    // — firmware version can change (an OTA update) and "last synced" is
    // meaningless across a restart — so they stay unpersisted: showing
    // "unknown"/blank for the few seconds before the next connect repopulates
    // them is honest rather than a real gap, same "don't show what you can't
    // verify" policy the device itself uses for presence.
    //
    // `cached_address`/`cached_serial_number`/`cached_manufacture_date` are
    // different: none of the three ever change for a given bond, so they're
    // ALSO write-through persisted to `store::SavedState` the first time
    // each is learned (`run_sync` for address, `read_device_settings` for
    // serial/mfg) and seeded back from disk at app launch
    // (`commands::get_initial_state`) — letting the Ori Info modal show them
    // even before this session's first connect completes. Cleared only when
    // the bond itself ends (`reset_session_caches`'s own doc comment).
    cached_fw_version: Mutex<Option<String>>,
    cached_address: Mutex<Option<String>>,
    cached_serial_number: Mutex<Option<String>>,
    cached_manufacture_date: Mutex<Option<String>>,
    // iPhone's resolved model name (PhoneBondStatus "d") — same write-through
    // treatment as the three identity fields above, write-through cached by
    // `phone_bond_watcher` and seeded back from disk at launch. See
    // `store::SavedState::phone_device_type`'s doc comment for why this
    // outlives an Orion disconnect (unlike the rest of `PhoneBondStatus`,
    // which the frontend correctly zeroes — a phone's model doesn't change
    // just because the *Ori* link dropped).
    cached_phone_device_type: Mutex<Option<String>>,
    last_synced: Mutex<Option<std::time::Instant>>,
    // MediaMetadata (char 000C, Write-WITH-Response — no Write-No-Response
    // property, unlike Album Art) and Media Album Art (char 000D, Write-NR)
    // need DIFFERENT concurrency strategies, because the BLE ATT protocol
    // itself treats them differently:
    //
    // Album Art (Write-No-Response) has no outstanding-request limit — the
    // host can fire fragments without waiting for a peripheral ack, so a
    // `tokio::task::JoinHandle::abort()` on the sending task genuinely stops
    // further fragments going out; a newer track's art can cleanly preempt
    // a still-streaming older one. `album_art_task` tracks that in-flight
    // task, aborted+replaced by `spawn_album_art_push`.
    //
    // MediaMetadata (Write-WITH-Response) is an ATT request/response
    // transaction, and the BLE spec allows only ONE such transaction
    // outstanding per connection at a time. Aborting the Rust task that
    // issued it does NOT retract an already-dispatched Write Request — Ori
    // still has to receive and ack it, and the OS's Bluetooth stack won't
    // even attempt the NEXT Write Request until that ack lands. An earlier
    // version of this fix used the same abort-and-restart pattern for
    // metadata too; on hardware this made a rapid burst of track changes
    // pile up a backlog of already-dispatched-but-abandoned Write Requests
    // that Ori had to plod through one by one, so even the LAST (wanted)
    // metadata write ended up waiting over a second behind stale acks — the
    // opposite of "immediately." `media_meta_push` instead uses a mailbox:
    // at most one Write Request in flight at a time; any track change that
    // arrives while one's outstanding just overwrites `pending` (coalescing
    // to the latest, never queuing multiple), and the in-flight write's own
    // task picks up whatever's newest in `pending` the moment its ack
    // arrives, looping until nothing newer is left. See `spawn_metadata_
    // push`'s doc comment for the implementation. Both are aborted+cleared
    // on disconnect in `start_post_sync_tasks` alongside the other
    // background tasks (both fields below: `task` aborted directly;
    // `running`/`pending` reset so a stale pending payload from the dead
    // connection is never sent to whatever connects next).
    album_art_task: Mutex<Option<tokio::task::JoinHandle<()>>>,
    media_meta_push: Mutex<MediaMetaPushState>,
}

#[derive(Default)]
struct MediaMetaPushState {
    // The worker task's own handle, purely so disconnect cleanup
    // (`start_post_sync_tasks`) can `.abort()` it directly — same rigor as
    // `album_art_task`. NOT used to decide whether to spawn a new worker
    // (see `running` below for why `JoinHandle::is_finished()` isn't safe
    // for that).
    task: Option<tokio::task::JoinHandle<()>>,
    // True while a worker task is alive draining this mailbox (i.e. either
    // currently awaiting a Write Request's ack, or about to loop back to
    // check `pending` again) — NOT the same as "a write is currently on the
    // wire," since the gap between one write's ack and checking `pending`
    // is real but short. Only ever read/written while holding the
    // `media_meta_push` mutex, together with `pending`, so there's no
    // window where a setter can miss a worker that's about to exit (see
    // `spawn_metadata_push`'s doc comment for why that atomicity matters —
    // and why this can't just be `task.as_ref().map_or(true, |t| t.is_
    // finished())`: a `JoinHandle` only reports finished AFTER the task
    // fully returns, which happens strictly after this same worker would
    // have already made its own "anything pending?" decision under this
    // lock — checking it from the outside can't be atomic with that
    // decision the way a single shared, lock-guarded bool can).
    running: bool,
    pending: Option<Vec<u8>>,
}

/// Firmware-default shortcut slot tokens — the combo a fresh device ships
/// with (media-mode.md). Single source for both `BleState::default` and
/// `reset_session_caches` so they can't drift apart.
fn default_shortcut_slots() -> [String; 3] {
    ["vol-mute".into(), "mic-mute".into(), "screenshot".into()]
}

impl Default for BleState {
    fn default() -> Self {
        Self {
            adapter: Mutex::default(),
            peripheral: Mutex::default(),
            pending_pin_tx: Mutex::default(),
            pairing_task: Mutex::default(),
            discover_task: Mutex::default(),
            shortcut_slots: Mutex::new(default_shortcut_slots()),
            favorite_combos: Mutex::new([Vec::new(), Vec::new(), Vec::new()]),
            sync_lock: Mutex::default(),
            background_tasks: Mutex::default(),
            ancs_tasks: Mutex::default(),
            device_name: Mutex::default(),
            supervisor_running: std::sync::atomic::AtomicBool::new(false),
            pairing_generation: std::sync::atomic::AtomicU64::new(0),
            reconnect_notify: tokio::sync::Notify::new(),
            disconnect_tx: tokio::sync::watch::channel(0).0,
            disconnect_watcher: Mutex::default(),
            supervisor_task: Mutex::default(),
            cached_fw_version: Mutex::default(),
            cached_address: Mutex::default(),
            cached_serial_number: Mutex::default(),
            cached_manufacture_date: Mutex::default(),
            cached_phone_device_type: Mutex::default(),
            last_synced: Mutex::default(),
            album_art_task: Mutex::default(),
            media_meta_push: Mutex::default(),
        }
    }
}

/// Clears the in-memory identity cache — `cached_address`/
/// `cached_serial_number`/`cached_manufacture_date`/`cached_phone_device_type`
/// — without touching shortcuts/combos. These four are write-through
/// persisted to `store::SavedState` (see `BleState`'s own doc comment), so
/// every caller pairs this with clearing the matching disk fields itself
/// (`saved.address = None` etc.) — without also clearing the in-memory side,
/// `get_ori_info` would keep answering with the old identity for the rest of
/// this running session, until the next app restart re-seeded from the
/// (by-then-cleared) disk copy. `cached_phone_device_type` belongs here too:
/// a factory reset / Clear All ends Ori's own bond, which per ble-protocol.md
/// §2 wipes its iPhone bond right along with it — same event, same treatment.
/// Split out from `reset_session_caches` below so `give_up_on_bond`
/// (commands.rs) — which ends a bond the same way a factory reset does, but
/// isn't a user-initiated "reset my shortcuts too" action — can clear
/// identity alone.
pub async fn clear_cached_identity(state: &BleState) {
    *state.cached_address.lock().await = None;
    *state.cached_serial_number.lock().await = None;
    *state.cached_manufacture_date.lock().await = None;
    *state.cached_phone_device_type.lock().await = None;
}

/// Clears just the iPhone's cached device type — a standalone `unpair_phone`
/// ends only the iPhone's bond, not Ori's own, so unlike `clear_cached_identity`
/// this leaves address/serial_number/manufacture_date untouched.
pub async fn clear_cached_phone_device_type(state: &BleState) {
    *state.cached_phone_device_type.lock().await = None;
}

/// Seeds the in-memory identity cache from `store::SavedState` at app
/// startup (`commands::get_initial_state`) — same idea as
/// `set_favorite_combos`'s own startup-seed call, just for these fields
/// instead. Lets `get_ori_info` answer with the last-known
/// address/serial/manufacture-date, and lets the iPhone Info modal show the
/// last-known device type, before this session's first connect ever
/// completes — which is the entire point of persisting them (pc-app.md).
pub async fn seed_cached_identity(state: &BleState, address: Option<String>, serial_number: Option<String>, manufacture_date: Option<String>, phone_device_type: Option<String>) {
    *state.cached_address.lock().await = address;
    *state.cached_serial_number.lock().await = serial_number;
    *state.cached_manufacture_date.lock().await = manufacture_date;
    *state.cached_phone_device_type.lock().await = phone_device_type;
}

/// Resets the per-session caches that outlive a single connection — back to
/// firmware defaults (`shortcut_slots`/`favorite_combos`), plus the identity
/// cache (`clear_cached_identity` above). Called on Factory Reset / Clear
/// All — both `commands.rs` call sites, the only two callers. Without the
/// shortcuts/combos reset, pairing the (now-wiped) device again in the same
/// app session would make `run_sync` push the previous owner's shortcut
/// tokens to Ori while the freshly-cleared store says firmware defaults,
/// leaving the two disagreeing until the next reconnect flip-flops it back.
pub async fn reset_session_caches(state: &BleState) {
    *state.shortcut_slots.lock().await = default_shortcut_slots();
    *state.favorite_combos.lock().await = [Vec::new(), Vec::new(), Vec::new()];
    clear_cached_identity(state).await;
}

/// Snapshot for the Ori Info/Stats modal (pc-app.md). Deliberately serves
/// only what's already cached — see `BleState`'s doc comment on
/// `cached_fw_version`/`cached_address`/`cached_serial_number`/
/// `cached_manufacture_date`/`last_synced` for why a fresh BLE read isn't
/// triggered here. `firmware_version`/`last_synced_secs_ago` are `None`
/// until this session's first connect completes (session-only, can't be
/// seeded from disk); `address`/`serial_number`/`manufacture_date` are
/// seeded from `store::SavedState` at launch (`get_initial_state`), so they
/// can already be populated here even before that. No RSSI/signal field:
/// btleplug's Windows backend only ever refreshes `PeripheralProperties.rssi`
/// from advertising packets, which stop the moment a peripheral is
/// connected — a "signal strength" shown here would just be the frozen
/// pre-connect reading, not a live one, so it's omitted rather than shown
/// misleadingly. (Live signal bars for the Ori Info modal instead ride
/// `read_device_settings`'s "r" field — Ori reports its own reading back,
/// ble-protocol.md §4/§6.4 — not this cached snapshot.)
#[derive(Serialize)]
pub struct OriInfo {
    name: String,
    firmware_version: Option<String>,
    address: Option<String>,
    serial_number: Option<String>,
    manufacture_date: Option<String>,
    last_synced_secs_ago: Option<u64>,
}

pub async fn get_ori_info(state: &BleState) -> OriInfo {
    OriInfo {
        name: state.device_name.lock().await.clone().unwrap_or_default(),
        firmware_version: state.cached_fw_version.lock().await.clone(),
        address: state.cached_address.lock().await.clone(),
        serial_number: state.cached_serial_number.lock().await.clone(),
        manufacture_date: state.cached_manufacture_date.lock().await.clone(),
        last_synced_secs_ago: state.last_synced.lock().await.map(|i| i.elapsed().as_secs()),
    }
}

/// Whether the currently-stored peripheral (if any) reports a live link.
/// Lets the supervisor adopt a connection something else already
/// established (e.g. a re-pair mid-backoff) instead of tearing it down with
/// a redundant `reconnect`.
pub async fn is_connected(state: &BleState) -> bool {
    match state.peripheral.lock().await.clone() {
        Some(p) => p.is_connected().await.unwrap_or(false),
        None => false,
    }
}

/// Best-effort disconnect of the stored peripheral if it's still up. The
/// supervisor calls this after a failed reconnect attempt so the next
/// attempt starts from a clean disconnected state: `reconnect` can fail
/// *after* the link is up (e.g. `run_sync` hits a sync-complete timeout
/// while the connection stays alive), and without this the supervisor's
/// `is_connected` adopt-check would then treat that live-but-unsynced link
/// as a fully-synced connection to adopt — showing "connected" with no sync
/// done and no background tasks running. Reserving the adopt path for a
/// genuinely externally-established connection (the re-pair race) means
/// forcing this link down first.
pub async fn force_disconnect(state: &BleState) {
    if let Some(p) = state.peripheral.lock().await.clone() {
        if p.is_connected().await.unwrap_or(false) {
            let _ = p.disconnect().await;
        }
    }
}

/// Atomically claims the "a supervisor loop is running" flag. Returns `true`
/// if this call won the claim (the caller should proceed); `false` if
/// another supervisor is already active. Paired with `release_supervisor`.
pub fn try_claim_supervisor(state: &BleState) -> bool {
    state
        .supervisor_running
        .compare_exchange(false, true, std::sync::atomic::Ordering::SeqCst, std::sync::atomic::Ordering::SeqCst)
        .is_ok()
}

pub fn release_supervisor(state: &BleState) {
    state.supervisor_running.store(false, std::sync::atomic::Ordering::SeqCst);
}

#[derive(Serialize, Deserialize, Clone, Default)]
pub struct ProfileInput {
    pub name: String,
    pub title: String,
    pub email: String,
    pub phone: String,
    // The setup wizard's crop tool already outputs exactly 228x228 JPEG
    // (openCrop(..., 1, 228, 228, true), quality 0.92) — matching
    // ble-protocol.md §10's Profile Photo spec — so there's no resize step
    // here, just decode-and-verify. `None` when no photo was picked.
    #[serde(rename = "photoDataUrl")]
    pub photo_data_url: Option<String>,
    // Absent on the initial-pairing call site (no pre-existing photo to
    // remove on a fresh device) — defaults to false there.
    #[serde(rename = "photoRemoved", default)]
    pub photo_removed: bool,
}

/// Mirrors the setup wizard's crop tool output for the Time Off destination
/// photo (528×396 JPEG, §10) — same decode-and-verify treatment as the
/// profile photo, just a different cap.
#[derive(Serialize, Deserialize, Clone, Default)]
pub struct TimeOffInput {
    pub start: i64,
    pub end: i64,
    pub destination: String,
    #[serde(rename = "photoDataUrl")]
    pub photo_data_url: Option<String>,
    #[serde(rename = "photoRemoved", default)]
    pub photo_removed: bool,
}

#[derive(Serialize, Clone)]
struct ScanResultEvent {
    name: String,
    strong: bool,
}

#[derive(Serialize, Clone)]
struct SyncProgressEvent {
    pct: f32,
    label: Option<&'static str>,
    done: bool,
}

type NotifyStream = Pin<Box<dyn futures::Stream<Item = ValueNotification> + Send>>;

const SCAN_DURATION: Duration = Duration::from_secs(6);
const RECONNECT_SCAN_TIMEOUT: Duration = Duration::from_secs(8);
const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
const SERVICE_DISCOVERY_TIMEOUT: Duration = Duration::from_secs(10);
/// Bounds `reconnect`'s encryption probe (the Device Settings read used to
/// force LE-SC encryption up from a stored LTK, §7.1). Unlike connect and
/// service discovery, this had no timeout at all — a stale-LTK peer that
/// hangs here (rather than failing outright with `BLE_HS_ENC_FAIL`) would
/// block the whole supervisor loop indefinitely with no way to recover
/// short of restarting Orion. Safety net on top of the `discover_named_device`
/// race fix (§ that function's own comment) — not expected to fire in the
/// common case, since that fix already keeps a freshly-reset (SETUP) Ori
/// from being treated as a reconnect target in the first place.
const ENCRYPTION_PROBE_TIMEOUT: Duration = Duration::from_secs(10);
const MANIFEST_REPLY_TIMEOUT: Duration = Duration::from_secs(10);
const SYNC_COMPLETE_TIMEOUT: Duration = Duration::from_secs(30);
/// ble-protocol.md §6.3: "Time Sync | Every 10 min | Write Time Sync (inside
/// BEGIN/END)". Ori has no battery-backed RTC (hardware.md) — without a
/// periodic resync its clock silently drifts for the length of any long
/// connected session.
const TIME_SYNC_REFRESH_INTERVAL: Duration = Duration::from_secs(600);

async fn get_adapter(state: &BleState) -> Result<Adapter, String> {
    let mut guard = state.adapter.lock().await;
    if let Some(adapter) = guard.as_ref() {
        return Ok(adapter.clone());
    }
    let manager = Manager::new().await.map_err(|e| e.to_string())?;
    let adapter = manager
        .adapters()
        .await
        .map_err(|e| e.to_string())?
        .into_iter()
        .next()
        .ok_or_else(|| "no Bluetooth adapter found on this PC".to_string())?;
    *guard = Some(adapter.clone());
    Ok(adapter)
}

/// Scans for `Ori-*` devices, emitting `scan-result` with the accumulated
/// list as each new one is found — `suRenderDevices()` just re-renders
/// whatever list it's handed, so resending the growing list on every
/// discovery is a drop-in incremental upgrade over the old
/// scan-then-emit-once-at-the-end behavior with no frontend changes needed.
///
/// Deliberately scans with an EMPTY `ScanFilter` — no service-UUID filter —
/// and does all filtering client-side by name prefix below. btleplug's
/// Windows backend applies a service-UUID filter per received advertisement
/// *event*, but Ori's advertising splits the device name (scan response)
/// and the Ori Sync Service UUID (primary packet) across two separate
/// events (ble-protocol.md §2 "Device name is in the scan response ...").
/// Filtering by service UUID drops the scan-response event before its name
/// ever reaches `local_name`, so every device silently fails the "Ori-"
/// check downstream — this is what caused scans to come back empty.
pub async fn scan(app: &AppHandle, state: &BleState) -> Result<(), String> {
    let adapter = get_adapter(state).await?;
    let mut events = adapter.events().await.map_err(|e| e.to_string())?;

    adapter
        .start_scan(ScanFilter::default())
        .await
        .map_err(|e| e.to_string())?;

    let mut confirmed = std::collections::HashSet::new();
    // Names seen so far (from DeviceDiscovered/DeviceUpdated), keyed by id —
    // consumed once a ManufacturerDataAdvertisement event for the same id
    // lets the SETUP-vs-RUNTIME decision be made straight from that event's
    // own payload. Deliberately NOT read from `peripheral.properties()`'s
    // merged `manufacturer_data`: btleplug only overwrites that field when a
    // packet containing manufacturer data arrives and never clears it
    // otherwise, so a device this same long-running Orion process has seen
    // before (e.g. earlier in this session, before Ori was reset) can report
    // a stale flag value left over from that earlier observation even once
    // a brand-new packet has freshly confirmed its name. The event payload
    // is always the packet that just arrived, never a merged leftover.
    let mut pending_names: std::collections::HashMap<PeripheralId, String> = std::collections::HashMap::new();
    let mut results: Vec<ScanResultEvent> = Vec::new();
    let deadline = tokio::time::sleep(SCAN_DURATION);
    tokio::pin!(deadline);

    loop {
        tokio::select! {
            maybe_event = events.next() => {
                let Some(event) = maybe_event else { break };
                match event {
                    CentralEvent::ManufacturerDataAdvertisement { id, manufacturer_data } => {
                        if confirmed.contains(&id) {
                            continue;
                        }
                        let Some(name) = pending_names.get(&id).cloned() else { continue };
                        // Only offer devices whose Orion/PC bond slot is
                        // actually open right now. The manufacturer-data mode
                        // flag is 0x02 RUNTIME in every state where the PC
                        // slot is already occupied and 0x01 SETUP in the two
                        // states where it's free (ble-protocol.md §2's
                        // advertising state machine table) — a RUNTIME device
                        // would just reject a new pairing attempt, so don't
                        // offer it as selectable.
                        let pairable = manufacturer_data
                            .get(&gatt::MFG_COMPANY_ID)
                            .and_then(|data| data.first())
                            .is_some_and(|&flag| flag == gatt::ADV_FLAG_SETUP);
                        if !pairable {
                            continue;
                        }
                        confirmed.insert(id.clone());
                        let strong = match adapter.peripheral(&id).await {
                            Ok(peripheral) => peripheral
                                .properties()
                                .await
                                .ok()
                                .flatten()
                                .and_then(|p| p.rssi)
                                .map(|rssi| rssi > -70)
                                .unwrap_or(false),
                            Err(_) => false,
                        };
                        results.push(ScanResultEvent { name, strong });
                        // `&results` (not `results.clone()`) — `Emitter::emit`'s
                        // bound is `Serialize + Clone`, and a shared reference is
                        // always cheaply `Copy`/`Clone` and serializes identically
                        // to the owned value, so this avoids a deep clone of the
                        // whole (monotonically growing) vector on every single
                        // discovery event.
                        let _ = app.emit("scan-result", &results);
                    }
                    CentralEvent::DeviceDiscovered(id) | CentralEvent::DeviceUpdated(id) => {
                        if confirmed.contains(&id) {
                            continue;
                        }
                        let Ok(peripheral) = adapter.peripheral(&id).await else { continue };
                        let Ok(Some(props)) = peripheral.properties().await else { continue };
                        let name = props
                            .local_name
                            .or(props.advertisement_name)
                            .unwrap_or_default();
                        if name.starts_with("Ori-") {
                            pending_names.insert(id, name);
                        }
                    }
                    _ => continue,
                }
            }
            _ = &mut deadline => break,
        }
    }

    adapter.stop_scan().await.map_err(|e| e.to_string())?;
    // Final emit covers the (rare) case of zero matches — `suRenderDevices`
    // needs at least one call to swap out of its "Scanning…" placeholder.
    if results.is_empty() {
        app.emit("scan-result", results).map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// Error sentinel from `discover_named_device`: the named device is
/// advertising the `SETUP` mode flag instead of `RUNTIME` — i.e. Ori's bonds
/// were wiped (a factory reset, possibly triggered locally on Ori's own
/// screen while Orion wasn't connected to see it happen — ble-protocol.md
/// §7.1). Our own Windows-level bond is now stale. The Bluetooth address is
/// appended after the colon so a caller can still remove that stale bond
/// without ever having connected. A plain string prefix rather than a
/// structured error type, to stay consistent with this module's existing
/// `Result<_, String>` error style throughout.
pub const ORI_FACTORY_RESET_PREFIX: &str = "__ori_factory_reset__:";

/// Error sentinel from `reconnect`: discovery found the expected device
/// (advertising normally, not `SETUP`), but a step after that — connecting,
/// discovering services, or the sync itself — failed. See `reconnect`'s doc
/// comment for why this is treated differently from a plain "not found."
/// Format matches `ORI_FACTORY_RESET_PREFIX`: address, then a colon, then
/// the underlying error message.
pub const ORI_POST_DISCOVERY_FAILURE_PREFIX: &str = "__ori_post_discovery_failure__:";

/// Actively scans for a specific already-known device by name — used by
/// `reconnect`, which (unlike the setup wizard's `scan`) can't rely on a
/// prior `ble_scan` call having already populated the adapter's discovered-
/// peripherals cache, and can't filter by the `SETUP` mode flag the way
/// `scan` does (a bonded device reconnecting normally advertises `RUNTIME`,
/// not `SETUP` — so here the flag is checked, not filtered on: seeing
/// `SETUP` on the device we expect to be bonded already is itself the
/// signal, see `ORI_FACTORY_RESET_PREFIX`).
async fn discover_named_device(adapter: &Adapter, name: &str, timeout: Duration) -> Result<Peripheral, String> {
    adapter.start_scan(ScanFilter::default()).await.map_err(|e| e.to_string())?;
    let mut events = adapter.events().await.map_err(|e| e.to_string())?;
    let deadline = tokio::time::sleep(timeout);
    tokio::pin!(deadline);

    enum Outcome {
        Found(Peripheral),
        FactoryReset(u64),
    }

    // Set once a DeviceDiscovered/DeviceUpdated event confirms the id's name
    // matches, consumed by the next ManufacturerDataAdvertisement event for
    // that same id — see the comment on that match arm for why the
    // SETUP-vs-RUNTIME decision is made from that event's own payload and
    // never from `peripheral.properties()`'s merged `manufacturer_data`.
    let mut name_matched: Option<PeripheralId> = None;

    let found = loop {
        tokio::select! {
            maybe_event = events.next() => {
                let Some(event) = maybe_event else { break None };
                match event {
                    CentralEvent::ManufacturerDataAdvertisement { id, manufacturer_data } => {
                        if name_matched.as_ref() != Some(&id) {
                            continue;
                        }
                        // Read straight from this event's own payload, not
                        // from `peripheral.properties()`: that field only
                        // gets overwritten when a packet containing
                        // manufacturer data arrives and is never cleared
                        // otherwise, so a device this same long-running
                        // Orion process has seen before (e.g. earlier in
                        // this session, before Ori was reset) can report a
                        // stale flag value left over from that earlier
                        // observation even once a brand-new packet has
                        // freshly confirmed its name via the arm below. This
                        // event's payload is always the packet that just
                        // arrived — never a merged leftover. (This was the
                        // actual bug behind Orion connecting to a freshly-
                        // reset, SETUP-advertising Ori as if it were still a
                        // bonded RUNTIME target and hanging forever on an
                        // encrypted read with no bond left to satisfy it —
                        // an earlier fix here only checked whether the field
                        // was *present*, which a stale-but-present value
                        // still satisfied.)
                        let is_reset = manufacturer_data
                            .get(&gatt::MFG_COMPANY_ID)
                            .and_then(|data| data.first())
                            .is_some_and(|&flag| flag == gatt::ADV_FLAG_SETUP);
                        let Ok(peripheral) = adapter.peripheral(&id).await else { continue };
                        break Some(if is_reset {
                            Outcome::FactoryReset(peripheral.address().into())
                        } else {
                            Outcome::Found(peripheral)
                        });
                    }
                    CentralEvent::DeviceDiscovered(id) | CentralEvent::DeviceUpdated(id) => {
                        let Ok(peripheral) = adapter.peripheral(&id).await else { continue };
                        let Ok(Some(props)) = peripheral.properties().await else { continue };
                        let candidate = props.local_name.or(props.advertisement_name).unwrap_or_default();
                        if candidate == name {
                            name_matched = Some(id);
                        }
                    }
                    _ => continue,
                }
            }
            _ = &mut deadline => break None,
        }
    };

    let _ = adapter.stop_scan().await;
    match found {
        Some(Outcome::Found(peripheral)) => Ok(peripheral),
        Some(Outcome::FactoryReset(address)) => Err(format!("{ORI_FACTORY_RESET_PREFIX}{address}")),
        None => Err(format!("{name} not found — is it powered on and in range?")),
    }
}

fn find_char(peripheral: &Peripheral, uuid_str: &str) -> Result<Characteristic, String> {
    let target = Uuid::parse_str(uuid_str).expect("valid UUID literal");
    peripheral
        .characteristics()
        .into_iter()
        .find(|c| c.uuid == target)
        .ok_or_else(|| format!("Ori didn't advertise characteristic {uuid_str}"))
}

/// Event-driven replacement for the old `is_connected()` sleep-poll used
/// throughout this file: subscribes to the adapter's event stream and bumps
/// `disconnect_tx` the instant btleplug relays a genuine link-layer
/// disconnect (`CentralEvent::DeviceDisconnected` — on Windows this is
/// wired straight to WinRT's own `BluetoothLEDevice.ConnectionStatusChanged`
/// event, `btleplug::winrtble::peripheral`, not a poll on btleplug's own
/// side either) for `peripheral_id` specifically. Exits once it's relayed
/// one disconnect — nothing more to watch for on a per-connection task — or
/// if the event stream itself ends.
async fn watch_for_disconnect(adapter: Adapter, peripheral_id: PeripheralId, disconnect_tx: tokio::sync::watch::Sender<u64>) {
    let Ok(mut events) = adapter.events().await else { return };
    while let Some(event) = events.next().await {
        if let CentralEvent::DeviceDisconnected(id) = event {
            if id == peripheral_id {
                disconnect_tx.send_modify(|v| *v = v.wrapping_add(1));
                return;
            }
        }
    }
}

/// Spawns `watch_for_disconnect` for the just-established `peripheral`,
/// aborting whatever instance was still tracked from a previous connection
/// first — a fast repeated reconnect must not leave two watchers matching
/// two different peripheral ids alive at once. Called once per connection,
/// from `subscribe_ancs_relay_early` — the earliest point after the link is
/// confirmed encrypted, common to both `reconnect` and `submit_passkey`.
/// Every other per-connection watcher in this file subscribes its own
/// `tokio::sync::watch::Receiver` from `state.disconnect_tx` independently
/// (no need to thread one through as a parameter) — a `watch` channel lets
/// any number of independent subscribers each reliably observe the next
/// change from wherever their own baseline was when they subscribed.
async fn spawn_disconnect_watcher(state: &BleState, adapter: &Adapter, peripheral: &Peripheral) {
    let mut slot = state.disconnect_watcher.lock().await;
    if let Some(prev) = slot.take() {
        prev.abort();
    }
    *slot = Some(tokio::spawn(watch_for_disconnect(adapter.clone(), peripheral.id(), state.disconnect_tx.clone())));
}

/// Snapshots the currently-connected peripheral, or a plain "not connected"
/// error — the shared tail of every command that needs a live link but isn't
/// itself in the middle of establishing one (those use their own tailored
/// error message instead, e.g. `submit_passkey`'s "select the device again").
async fn require_peripheral(state: &BleState) -> Result<Peripheral, String> {
    state.peripheral.lock().await.clone().ok_or_else(|| "not connected".to_string())
}

/// Best-effort connection-interval/latency request (maps to WinRT's
/// `RequestPreferredConnectionParameters` on Windows — btleplug has no
/// generic PHY or ATT-MTU request API on any backend; the OS negotiates
/// both automatically and doesn't expose an app-level override — see
/// `ble-protocol.md` §5's note and this function's callers). Requesting a
/// preset is the closest real lever to "pack more into each connection
/// event": `ThroughputOptimized` (short interval, low latency) is requested
/// around the one genuinely large transfer — the initial/reconnect sync,
/// which can carry a profile photo and/or Time Off image on top of the
/// meeting list — and `PowerOptimized` (long interval, high slave latency)
/// once the connection settles into steady state, so Ori and Orion aren't
/// servicing frequent connection events for a link that's mostly idle.
/// Errors are swallowed: some Bluetooth radios/drivers don't support the
/// request at all (`NotSupported`), and a rejected preference doesn't
/// change anything about correctness — the sync/bridge logic doesn't depend
/// on which interval is actually in effect.
async fn set_connection_priority(peripheral: &Peripheral, preset: ConnectionParameterPreset) {
    let _ = peripheral.request_connection_parameters(preset).await;
}

/// Connects to `name` and kicks off WinRT's custom-passkey pairing ceremony
/// (ble-protocol.md §6.1, `ble::pairing`) — this is what actually causes
/// Ori to generate and display its 6-digit code, so it must run as soon as
/// the connection is up, with nothing else on the critical path in front of
/// it. Returns once connected; pairing itself continues in the background
/// until `submit_passkey` supplies the code the user reads off Ori's
/// screen — and if it fails or Ori gives up before that happens, this emits
/// `pairing-failed` itself so the UI doesn't sit on a dead passkey screen
/// waiting for input that can no longer succeed.
pub async fn start_pairing(app: &AppHandle, state: &BleState, name: &str) -> Result<(), String> {
    // Defensive cleanup — tear down whatever attempt (if any) is still
    // registered before this one overwrites `pending_pin_tx`/`peripheral`/
    // `discover_task`/`pairing_task`/`device_name` below. Without this,
    // re-invoking pairing before a previous attempt was cancelled (a double
    // "Pair" click, or backing out to the device list and picking a
    // *different* Ori without hitting Cancel first) would silently orphan
    // the old attempt's parked blocking-pool thread, its still-open BLE
    // connection, and its disconnect-watcher polling task — forever, since
    // nothing referencing the old state would be left to ever clean it up.
    // See `teardown_pairing_attempt`'s doc comment for why this is safe to
    // call unconditionally (a no-op when there's nothing to tear down).
    teardown_pairing_attempt(state).await;

    let adapter = get_adapter(state).await?;

    let mut found: Option<Peripheral> = None;
    for peripheral in adapter.peripherals().await.map_err(|e| e.to_string())? {
        if let Ok(Some(props)) = peripheral.properties().await {
            let candidate = props.local_name.or(props.advertisement_name).unwrap_or_default();
            if candidate == name {
                found = Some(peripheral);
                break;
            }
        }
    }
    let peripheral = found.ok_or_else(|| format!("{name} wasn't found — scan again"))?;

    peripheral
        .connect_with_timeout(CONNECT_TIMEOUT)
        .await
        .map_err(|e| format!("couldn't connect: {e}"))?;

    let bluetooth_address: u64 = peripheral.address().into();
    // Identifies *this* attempt so its eventual result can be recognized as
    // stale if `cancel_pairing` (or a fresh `start_pairing`) supersedes it
    // before it resolves — see `pairing_generation`'s doc comment.
    let generation = state.pairing_generation.fetch_add(1, std::sync::atomic::Ordering::SeqCst) + 1;
    let (tx, rx) = std::sync::mpsc::channel::<pairing::PairingInput>();
    *state.pending_pin_tx.lock().await = Some(tx.clone());
    *state.peripheral.lock().await = Some(peripheral.clone());
    *state.device_name.lock().await = Some(name.to_string());

    // Service discovery doesn't need the link to be paired/encrypted — GATT
    // structure (UUIDs, properties) is public metadata; only reading or
    // writing an encrypted characteristic's *value* requires the bond. So
    // it runs concurrently with pairing instead of blocking PairAsync from
    // starting — `submit_passkey` awaits this before it needs the
    // discovered characteristics for `run_sync`.
    let discover_peripheral = peripheral.clone();
    let discover_task = tokio::spawn(async move {
        discover_peripheral
            .discover_services_with_timeout(SERVICE_DISCOVERY_TIMEOUT)
            .await
            .map_err(|e| format!("couldn't discover services: {e}"))
    });
    *state.discover_task.lock().await = Some(discover_task);

    // No independent timeout on our side — see `ble::pairing`'s module
    // comment. Instead, watch for the one thing that actually signals Ori
    // gave up: the link dropping. Stops as soon as either that happens or
    // the pairing attempt finishes on its own (pin submitted, accepted or
    // rejected) — `done_rx` firing means `pin_rx` inside the WinRT handler
    // is no longer listening, so there'd be nothing left to cancel anyway.
    let (done_tx, mut done_rx) = tokio::sync::oneshot::channel::<()>();
    let app_for_task = app.clone();
    let task = tokio::spawn(async move {
        let result = pairing::pair_with_passkey(bluetooth_address, rx).await;
        let _ = done_tx.send(());
        // The user may still be staring at an empty passkey screen with
        // nothing to submit — don't wait for `submit_passkey` to ever be
        // called before surfacing this. But only if this is still the
        // live attempt: if the user already cancelled (or started a new
        // one), `pairing_generation` has moved past `generation`, and this
        // result — unblocked by `cancel_pairing`'s own `Disconnected` send —
        // is stale and must not pop a failure dialog over whatever's
        // on-screen now.
        if let Err(reason) = &result {
            let current = app_for_task.state::<BleState>().pairing_generation.load(std::sync::atomic::Ordering::SeqCst);
            if current == generation {
                let _ = app_for_task.emit("pairing-failed", reason.clone());
            }
        }
        result
    });
    *state.pairing_task.lock().await = Some(task);

    // Event-driven, not polled: watches the adapter's event stream directly
    // for btleplug's `CentralEvent::DeviceDisconnected` (WinRT's own
    // `ConnectionStatusChanged` on this backend) matching this specific
    // peripheral, instead of sleep-polling `is_connected()`.
    let peripheral_id = peripheral.id();
    tokio::spawn(async move {
        let Ok(mut events) = adapter.events().await else { return };
        loop {
            tokio::select! {
                maybe_event = events.next() => {
                    match maybe_event {
                        Some(CentralEvent::DeviceDisconnected(id)) if id == peripheral_id => {
                            let _ = tx.send(pairing::PairingInput::Disconnected);
                            return;
                        }
                        Some(_) => continue,
                        None => return,
                    }
                }
                _ = &mut done_rx => return,
            }
        }
    });

    Ok(())
}

/// Tears down whatever pairing attempt is currently registered in `state`,
/// if any: unblocks the blocking-pool thread parked in
/// `pair_with_passkey_blocking`'s `pin_rx.recv()` (via a `Disconnected`
/// send), aborts the concurrent service-discovery task and the outer
/// pairing-task handle, and disconnects + clears the stale peripheral and
/// device name. Shared by `cancel_pairing` (user-initiated Cancel) and
/// `start_pairing` (defensive cleanup before starting a new attempt) so
/// neither path can leak a previous attempt's parked thread + open BLE
/// connection + polling task — both self-heal eventually once Ori's own
/// ~30s NimBLE pairing-procedure timeout drops the link, but there's no
/// reason to wait on that once something has superseded the attempt, and
/// repeated cancel-and-retry (or re-picking a device) during setup would
/// otherwise pile these up one per attempt.
///
/// **Race safety, gated on winning `pending_pin_tx`'s `take()`:** whichever
/// caller succeeds in taking it becomes the sole owner of tearing down the
/// rest of that attempt's state (`pairing_task`/`discover_task`/
/// `peripheral`/`device_name`, and the only one that bumps
/// `pairing_generation`) — a caller that loses the race finds `None` and
/// returns immediately, touching nothing else. This is what closes the race
/// between `cancel_pairing` and a concurrent `submit_passkey`: `submit_passkey`
/// only proceeds past its own `pending_pin_tx.take()` when *it* wins that same
/// race, so the two functions can never fight over one attempt's
/// `pairing_task`/`peripheral` — a `cancel_pairing` that arrives after the PIN
/// has already been sent (i.e. after `submit_passkey` won) finds
/// `pending_pin_tx` already empty and backs off instead of ripping away state
/// `submit_passkey` (or a bond Ori just formed in the background) still needs.
/// Previously `cancel_pairing` bumped `pairing_generation` and cleared
/// `pairing_task`/`discover_task` unconditionally (not gated on the same
/// take), so it could win that scramble against an in-flight `submit_passkey`
/// and discard a possibly-successful bond outcome; gating everything on one
/// shared `take()` makes that structurally impossible instead of relying on
/// timing.
async fn teardown_pairing_attempt(state: &BleState) {
    let Some(tx) = state.pending_pin_tx.lock().await.take() else {
        return;
    };
    // Bumped only now that we know we actually own an attempt to tear down —
    // an unconditional bump (the old behavior) could mark a live,
    // submit_passkey-owned attempt as stale even when nothing about it was
    // actually cancelled.
    state.pairing_generation.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    let _ = tx.send(pairing::PairingInput::Disconnected);
    if let Some(task) = state.discover_task.lock().await.take() {
        task.abort();
    }
    // `pairing_task` runs the blocking WinRT ceremony via `spawn_blocking` —
    // `.abort()` can't preempt that blocking code, but the `Disconnected`
    // send above already unblocks `pin_rx.recv()`, so the task resolves (and
    // the disconnect-watcher spawned alongside it exits via `done_rx`) on its
    // own within microseconds regardless. `.abort()` is still applied here
    // (rather than just dropping the handle) so the outer async task itself
    // is cancelled promptly instead of being left to run — and hold its
    // WinRT/COM state — until that natural unblock.
    if let Some(task) = state.pairing_task.lock().await.take() {
        task.abort();
    }
    if let Some(p) = state.peripheral.lock().await.take() {
        let _ = p.disconnect().await;
    }
    *state.device_name.lock().await = None;
}

/// Aborts an in-flight `start_pairing` ceremony the user backed out of
/// (passkey modal Cancel) before ever submitting a code. Delegates to
/// `teardown_pairing_attempt`, shared with `start_pairing`'s own defensive
/// cleanup — see that function's doc comment for how the two race safely
/// against a concurrent `submit_passkey`.
pub async fn cancel_pairing(state: &BleState) {
    teardown_pairing_attempt(state).await;
}

/// Feeds the passkey the user read off Ori's screen into the pairing
/// ceremony `start_pairing` already has open, waits for pairing to
/// complete, then runs the full sync.
///
/// Winning the `pending_pin_tx.take()` below is what gives this call
/// exclusive ownership of `pairing_task`/`discover_task`/`peripheral` for the
/// rest of the function — see `teardown_pairing_attempt`'s doc comment for
/// why a concurrent `cancel_pairing` can never steal them out from under a
/// `submit_passkey` that already sent the PIN.
pub async fn submit_passkey(app: &AppHandle, state: &BleState, passkey: &str, profile: &ProfileInput) -> Result<(), String> {
    let tx = state
        .pending_pin_tx
        .lock()
        .await
        .take()
        .ok_or_else(|| "no pairing in progress — select the device again".to_string())?;
    // A closed receiver (pairing already failed/gave up) just means the
    // pairing_task join below will surface the real error.
    let _ = tx.send(pairing::PairingInput::Pin(passkey.to_string()));

    let task = state
        .pairing_task
        .lock()
        .await
        .take()
        .ok_or_else(|| "no pairing in progress — select the device again".to_string())?;
    task.await.map_err(|e| format!("pairing task panicked: {e}"))??;

    // Started concurrently with pairing in `start_pairing` — normally long
    // finished by now, but wait for it rather than assume.
    let discover_task = state
        .discover_task
        .lock()
        .await
        .take()
        .ok_or_else(|| "no pairing in progress — select the device again".to_string())?;
    discover_task.await.map_err(|e| format!("service discovery task panicked: {e}"))??;

    let peripheral = state
        .peripheral
        .lock()
        .await
        .clone()
        .ok_or_else(|| "not connected — select the device again".to_string())?;
    // Subscribe to the ANCS relay as early as the now-bonded, encrypted link
    // allows — before run_sync's slower sync exchange — see
    // subscribe_ancs_relay_early's doc comment. First pairing isn't the
    // reconnect race that comment is really about (no persisted CCCD for
    // NimBLE to auto-restore on a brand-new bond), but doing it here too
    // keeps both paths identical and is strictly safer either way.
    subscribe_ancs_relay_early(app, state, &peripheral).await?;

    // Nothing set yet on a genuine first pair — the zeroed default is
    // honest here (matches Ori's own "no Time Off" NVS-empty state).
    // `state.shortcut_slots` is still at `BleState::default()`'s combo,
    // which is exactly the firmware-default combo we want on a fresh pair.
    run_sync(app, state, &peripheral, profile, &TimeOffInput::default(), true).await?;

    start_post_sync_tasks(app, state, peripheral).await;

    Ok(())
}

/// Bonded reconnect (ble-protocol.md §6.2) — used on app launch when
/// `store::load()` says we're already paired. No passkey ceremony at all:
/// the LTK bond already lives in the OS's own Bluetooth stack and survives
/// an Orion restart on its own; this is purely Orion re-establishing its
/// own BLE connection and re-running the same hash-manifest sync `run_sync`
/// already uses for both first-pair and reconnect.
///
/// `profile` and `time_off` MUST be the last values Orion successfully
/// synced (cached in `store::SavedState`), not blank ones — `run_sync`
/// hashes whatever it's given to ask Ori "do you already have this," and
/// blank data hashes differently from Ori's real stored data, which would
/// make Ori report it as "needed" and get overwritten with empty fields.
///
/// `shortcuts` has the same hazard as `profile`/`time_off`, just outside the
/// hash-manifest mechanism: `run_sync` sends whatever's in
/// `state.shortcut_slots` unconditionally on every sync (§6.3 "Shortcuts —
/// always"), so it must be seeded from the last value Orion actually wrote,
/// or a reconnect would silently reset the user's chosen icons back to the
/// firmware default.
///
/// Errors up through the first *encrypted* GATT operation — connect, service
/// discovery, and the encryption probe below — are prefixed with
/// `ORI_POST_DISCOVERY_FAILURE_PREFIX`. Discovery finding the device
/// (advertising normally, not `SETUP`) but the encrypted link then failing to
/// establish is what §7.1's stale-LTK case looks like (a bond Windows still
/// considers valid but Ori has forgotten): it's meaningfully different from a
/// plain "not found" (Ori simply unreachable — powered off, out of range —
/// which is no evidence against the bond). See `commands::supervise_connection`
/// for how repeated occurrences of this prefix are handled.
///
/// Crucially, `run_sync` is NOT wrapped: it runs *after* encryption is proven
/// working, so a failure inside it (a sync-complete timeout, a chunk-transfer
/// double-NACK give-up) is a sync-logic problem, not a dead bond — counting
/// those toward the stale-bond threshold would force a pointless re-pair that
/// fixes nothing. The probe read is the boundary that separates the two.
pub async fn reconnect(
    app: &AppHandle,
    state: &BleState,
    name: &str,
    profile: &ProfileInput,
    time_off: &TimeOffInput,
    shortcuts: &[String; 3],
) -> Result<(), String> {
    let adapter = get_adapter(state).await?;
    let peripheral = discover_named_device(&adapter, name, RECONNECT_SCAN_TIMEOUT).await?;
    let address: u64 = peripheral.address().into();
    let wrap = |e: String| format!("{ORI_POST_DISCOVERY_FAILURE_PREFIX}{address}:{e}");
    // Ori is found — Disconnected → Connecting. Emitted here rather than by
    // the supervisor loop up front (the old scheme) so the UI only ever
    // advances on genuine progress: a scan that never finds anything leaves
    // the state exactly where it was instead of guessing "Connecting" before
    // there's anything to connect to.
    let _ = app.emit("conn-state", "connecting");

    peripheral
        .connect_with_timeout(CONNECT_TIMEOUT)
        .await
        .map_err(|e| wrap(format!("couldn't connect: {e}")))?;
    peripheral
        .discover_services_with_timeout(SERVICE_DISCOVERY_TIMEOUT)
        .await
        .map_err(|e| wrap(format!("couldn't discover services: {e}")))?;

    // Encryption probe — the true §7.1 boundary. Connect and service discovery
    // don't require an encrypted link (the GATT table is public metadata), so
    // a stale LTK doesn't surface until the first *encrypted* characteristic
    // access. Reading Device Settings (000E, encrypted + readable, and read on
    // reconnect anyway per §6.4) forces LE-SC link encryption up from the
    // stored LTK; a rejected bond fails here (INSUFFICIENT_AUTHENTICATION /
    // BLE_HS_ENC_FAIL). Everything from here on is proven-encrypted, so it's
    // left unwrapped.
    let chr_settings = find_char(&peripheral, gatt::CHR_DEVICE_SETTINGS).map_err(&wrap)?;
    tokio::time::timeout(ENCRYPTION_PROBE_TIMEOUT, peripheral.read(&chr_settings))
        .await
        .map_err(|_| wrap("encrypted read timed out (stale bond?)".to_string()))?
        .map_err(|e| wrap(format!("encrypted read failed (stale bond?): {e}")))?;

    *state.peripheral.lock().await = Some(peripheral.clone());
    *state.device_name.lock().await = Some(name.to_string());
    *state.shortcut_slots.lock().await = shortcuts.clone();

    // Subscribe to the ANCS relay THE MOMENT the link is proven-encrypted —
    // before run_sync's slower Device Settings/Manifest/BEGIN-END exchange
    // even starts. This is the reconnect-specific race subscribe_ancs_relay_
    // early's doc comment covers in full: on a bonded reconnect, NimBLE can
    // fire Ori's onSubscribe()-triggered resync (replaying the whole current
    // ANCS queue / call state) from ITS OWN persisted CCCD state the instant
    // encryption comes up — well before Orion's code would otherwise get
    // around to subscribing, which used to be only after the entire sync
    // below completed. Missing that window drops the resync permanently for
    // this connection (aggregate counts still read correctly later; the
    // underlying list/call state does not).
    subscribe_ancs_relay_early(app, state, &peripheral).await.map_err(&wrap)?;

    // Connection established and proven-encrypted — Connecting → Syncing,
    // right before the real sync work (`run_sync`) begins.
    let _ = app.emit("conn-state", "rec");
    run_sync(app, state, &peripheral, profile, time_off, false).await?;

    start_post_sync_tasks(app, state, peripheral).await;

    Ok(())
}

/// Kicks off everything that should keep running for the rest of a live
/// connection once the initial (or reconnect) sync has completed: the
/// Controls-mode media/volume bridge (§12), the Phone Bond Status watcher
/// (§3/§11), a one-shot firmware-version check (§3.1/§9), and the periodic
/// Time Sync refresh (§6.3).
///
/// Aborts any tasks left over from a *previous* connection first — each of
/// these self-exits event-driven now (`disconnect_tx`, `watch_for_disconnect`'s
/// doc comment), but a fast back-to-back reconnect can still race that
/// signal itself (see `BleState::background_tasks`'s doc comment for the
/// exact window), leaving the old task running alongside its replacement —
/// the concrete failure mode this caused was doubled media commands.
///
/// The two ANCS relay watcher tasks are NOT spawned here — `subscribe_ancs_
/// relay_early` already started them, well before this function runs, and
/// tracks them separately in `state.ancs_tasks` rather than
/// `state.background_tasks` below. See that function's doc comment for why:
/// in short, they can't wait this long to start draining their receivers
/// without risking the underlying notify buffer overflowing first.
async fn start_post_sync_tasks(app: &AppHandle, state: &BleState, peripheral: Peripheral) {
    {
        let mut tasks = state.background_tasks.lock().await;
        for task in tasks.drain(..) {
            task.abort();
        }
    }
    // Both media push mechanisms (spawn_album_art_push / spawn_metadata_push)
    // are spawned dynamically by media_bridge, not tracked in
    // background_tasks above — reset/abort any left running from a previous
    // connection here too, so neither can linger writing to a now-dead
    // peripheral, or hand a stale pending payload to whatever connects next,
    // across a fast reconnect.
    if let Some(prev) = state.album_art_task.lock().await.take() {
        prev.abort();
    }
    {
        let mut mp = state.media_meta_push.lock().await;
        if let Some(prev) = mp.task.take() {
            prev.abort();
        }
        mp.running = false;
        mp.pending = None;
    }

    // The bulk transfer `run_sync` just finished (or, on a delta reconnect
    // with nothing needed, never really needed the short interval anyway) —
    // settle to the low-overhead preset for the rest of this connection's
    // steady state (see `set_connection_priority`'s doc comment).
    set_connection_priority(&peripheral, ConnectionParameterPreset::PowerOptimized).await;

    // Takes `AppHandle` (not `&BleState` — a Tauri-managed-state borrow
    // isn't `'static`) and re-reads `state.shortcut_slots` from it on every
    // `shortcut` dispatch, so a slot changed mid-connection is picked up
    // immediately rather than frozen at connect time.
    let tasks = vec![
        tokio::spawn(check_firmware_version(app.clone(), peripheral.clone())),
        tokio::spawn(phone_bond_watcher(app.clone(), peripheral.clone())),
        tokio::spawn(device_settings_watcher(app.clone(), peripheral.clone())),
        tokio::spawn(time_sync_refresher(app.clone(), peripheral.clone())),
        tokio::spawn(media_bridge(app.clone(), peripheral.clone())),
    ];
    *state.background_tasks.lock().await = tasks;

    // Pull-based ANCS relay resync ("RSYN", Device Command char 0008 —
    // ble-protocol.md §13): explicitly ask Ori to replay its full ANCS
    // mirror (chars 0010/0011) NOW. This is the one trigger that's correct
    // by construction: the relay watchers (`subscribe_ancs_relay_early`)
    // have provably been subscribed AND actively draining since before
    // `run_sync` even started, so the replay this write provokes cannot be
    // missed. It exists because every push-timed resync lost a race on
    // bonded reconnects — Ori's onSubscribe() fires from NimBLE's own
    // bonding-restore the instant encryption resumes, before Orion is
    // listening (pc-app.md's full writeup) — and no server-side "late
    // enough" guess is robust. Best-effort: older firmware NACKs the
    // unknown magic (write error ignored), harmlessly leaving the old
    // push-timed behavior in place.
    if let Ok(chr_cmd) = find_char(&peripheral, gatt::CHR_DEVICE_COMMAND) {
        let _ = peripheral.write(&chr_cmd, &gatt::RESYNC_ANCS_MAGIC, WriteType::WithResponse).await;
    }
}

/// §6.3's periodic Time Sync refresh — re-sends just Time Sync, in its own
/// scoped BEGIN/END session, every `TIME_SYNC_REFRESH_INTERVAL` for as long
/// as the link stays up. Exits immediately on disconnect (event-driven via
/// `state.disconnect_tx`) rather than waiting out the rest of the current
/// 10-minute tick.
async fn time_sync_refresher(app: AppHandle, peripheral: Peripheral) {
    let mut tick = tokio::time::interval(TIME_SYNC_REFRESH_INTERVAL);
    tick.tick().await; // first tick fires immediately — run_sync/reconnect just sent one
    let mut disconnect_rx = app.state::<BleState>().disconnect_tx.subscribe();
    loop {
        tokio::select! {
            _ = tick.tick() => {}
            _ = disconnect_rx.changed() => return,
        }
        let state = app.state::<BleState>();
        let _ = push_time_sync_only(&state, &peripheral).await;
    }
}

/// Pushes just Time Sync through its own minimal BEGIN/END session — the
/// same mechanism `push_profile`/`push_time_off` use, scoped down to the one
/// characteristic (§6.3's periodic refresh doesn't touch anything else).
async fn push_time_sync_only(state: &BleState, peripheral: &Peripheral) -> Result<(), String> {
    let chr_status = find_char(peripheral, gatt::CHR_DEVICE_STATUS)?;
    let chr_time = find_char(peripheral, gatt::CHR_TIME_SYNC)?;
    let chr_syncctl = find_char(peripheral, gatt::CHR_SYNC_CONTROL)?;

    let _sync_guard = state.sync_lock.lock().await;

    peripheral.subscribe(&chr_status).await.map_err(|e| e.to_string())?;
    let mut notifications: NotifyStream = peripheral.notifications().await.map_err(|e| e.to_string())?;

    let time_sync_bytes = cbor::encode(&cbor::TimeSync {
        u: now_epoch_secs(),
        z: &local_posix_tz(),
        x: process_clock_ms(),
    });

    peripheral
        .write(
            &chr_syncctl,
            &cbor::encode(&cbor::SyncControlBegin::new(MID_SESSION_SEQ, time_sync_bytes.len() as u64)),
            WriteType::WithResponse,
        )
        .await
        .map_err(|e| e.to_string())?;
    peripheral
        .write(&chr_time, &time_sync_bytes, WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;
    peripheral
        .write(&chr_syncctl, &cbor::encode(&cbor::SyncControlEnd::new(MID_SESSION_SEQ)), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;

    wait_for_sync_complete(&mut notifications, &chr_status, SYNC_COMPLETE_TIMEOUT).await
}

/// How often `media_bridge` falls back to polling now-playing state — only
/// used at all when `media::watch_now_playing` (GSMTC's own change events)
/// couldn't be set up in the first place. Volume no longer has (or needs) a
/// poll fallback here: `media::watch_volume` wraps WASAPI's own
/// `IAudioEndpointVolumeCallback` push notification, which — unlike GSMTC —
/// has no separate "just poll this instead" primitive to fall back to if
/// setup fails; see `media_bridge`'s `_volume_watcher` comment.
const NOW_PLAYING_FALLBACK_POLL_INTERVAL: Duration = Duration::from_millis(6000);

/// pc-app.md's PC-side seek correction: "if position deviates > ~5 s from
/// dead-reckoned while playing, push corrected MediaMetadata with
/// position_s/duration_s." Ori advances its own position display on a local
/// 1 s tick between pushes rather than polling Orion continuously, so a
/// user seeking in the OS player needs an explicit correction push to catch
/// Ori's display back up.
const SEEK_CORRECTION_THRESHOLD_SECS: u64 = 5;

/// `run_album_art_push`'s cap on how long it waits for an outstanding
/// MediaMetadata write to drain before starting its own write anyway (see
/// that wait's own doc comment for why it waits at all). A normal metadata
/// write acks in tens of milliseconds; this is generous headroom for a
/// slow-but-still-working connection while still bounding the worst case —
/// a metadata write that's genuinely stuck (rather than just contending for
/// airtime) must not block album art forever.
const ART_METADATA_WAIT_TIMEOUT: Duration = Duration::from_millis(1500);

/// Bounds a single MediaMetadata `WriteType::WithResponse` call. Found on
/// hardware (2026-07-13): this write occasionally hangs far past its normal
/// ~35-55ms — observed 4.1s and, in a second capture, 11.18s — right after
/// back-to-back rapid track-change swipes, with no OS-level timeout of its
/// own guarding it (`btleplug`'s Windows backend rides WinRT's
/// `WriteValueWithResultAsync`, which is known to occasionally stall well
/// past any reasonable per-write budget). Without this bound, one hung write
/// holds `MediaMetaPushState::running` true for its entire stall, which (a)
/// silently coalesces every intervening track change into `pending` without
/// ever sending one, and (b) forces every concurrent album-art push to sit
/// out its own `ART_METADATA_WAIT_TIMEOUT` before giving up and contending
/// with the same still-stuck connection anyway. This does NOT cancel the
/// underlying OS-level write — dropping the future can't retract a
/// Write Request already handed to the BLE stack, same limitation as
/// `spawn_album_art_push`'s doc comment describes for its own abort — it
/// only stops *this task* from waiting on it, freeing the mailbox to
/// immediately attempt the newest queued payload instead of staying wedged
/// for the full stall.
const METADATA_WRITE_TIMEOUT: Duration = Duration::from_millis(2000);

/// Per-connection now-playing state `check_and_push_now_playing` tracks
/// across calls, however it's triggered (a GSMTC change ping or, absent
/// that, the fallback poll) — pulled into its own struct so `media_bridge`
/// doesn't have to thread four separate `&mut` locals through both call
/// sites.
#[derive(Default)]
struct NowPlayingState {
    last_track: (String, String, bool),
    // SHA-256 of the raw OS thumbnail bytes last actually pushed (or `None`
    // if the current track has no art) — the ONLY signal that gates a new
    // album-art push. Deliberately independent of `last_track`/metadata:
    // many players deliver the thumbnail in a second wave after the initial
    // title/artist (the SMTC session is created immediately for
    // responsiveness, art follows once fetched from network/disk), and some
    // sources (live radio, some podcast apps) rotate art while title/artist
    // stay static. Gating art on "did title/artist change" missed both —
    // silently, since the old code nested the art check entirely inside the
    // metadata-changed branch. Content-hashing the actual bytes catches a
    // real art change no matter what metadata did or didn't do.
    last_art_hash: Option<Vec<u8>>,
    // (position_s at last push, epoch seconds at that push) — the baseline
    // for detecting drift between Ori's own dead-reckoned position and
    // reality (see `SEEK_CORRECTION_THRESHOLD_SECS`).
    last_pushed_position: Option<(u32, u64)>,
}

/// Fetches current now-playing state and pushes metadata and album art to
/// Ori as two fully independent deltas — the reusable body behind both
/// `media_bridge`'s event-driven path (a GSMTC change ping) and its
/// fallback poll (only used if `media::watch_now_playing` couldn't be set
/// up on this system). `check_art` is false for a ping that GSMTC's own API
/// scope guarantees can't carry an art change (`PlaybackInfoChanged`/
/// `TimelinePropertiesChanged` — see `media::NowPlayingChange`'s doc
/// comment) — skipping the OS thumbnail fetch entirely on those saves the
/// round-trip on every play/pause/seek. Metadata is always checked
/// regardless of `check_art`.
async fn check_and_push_now_playing(
    app: &AppHandle,
    peripheral: &Peripheral,
    chr_media: &Characteristic,
    chr_art: Option<&Characteristic>,
    np: &mut NowPlayingState,
    check_art: bool,
) {
    match media::now_playing().await {
        Ok(Some(now)) => {
            let identity_changed = (now.title.as_str(), now.artist.as_str()) != (np.last_track.0.as_str(), np.last_track.1.as_str());
            let track = (now.title.clone(), now.artist.clone(), now.playing);
            if track != np.last_track {
                if identity_changed {
                    // Kill any transfer still streaming the track we're
                    // LEAVING right now — before the metadata push, and
                    // before check_and_push_album_art's own (comparatively
                    // slow) OS thumbnail fetch+hash for the new track. See
                    // `abort_album_art_task`'s doc comment for why this
                    // can't just wait for that function to get around to it.
                    abort_album_art_task(app).await;
                }
                np.last_track = track;
                np.last_pushed_position = now.position_s.map(|p| (p, now_epoch_secs()));
                let payload = encode_media_metadata_payload(&now, true);
                // Metadata (mailbox-coalesced, `spawn_metadata_push`) and art
                // (abort-and-restart, `spawn_album_art_push`) are pushed
                // through TWO INDEPENDENT, non-blocking calls — never one
                // combined blocking sequence. See each function's own doc
                // comment for why they need different concurrency strategies
                // (Write-WITH-Response vs Write-No-Response have different
                // ATT-level outstanding-request rules). Neither call here
                // awaits the underlying write completing, only the (cheap)
                // dispatch — so this whole branch returns immediately
                // regardless of how long either write actually takes,
                // keeping `media_bridge`'s loop free to react to the next
                // change right away.
                spawn_metadata_push(app, peripheral, chr_media, payload).await;
            } else if now.playing {
                if let (Some(actual), Some((last_pos, last_at))) = (now.position_s, np.last_pushed_position) {
                    let elapsed = now_epoch_secs().saturating_sub(last_at);
                    let expected = last_pos as u64 + elapsed;
                    let deviation = (actual as i64 - expected as i64).unsigned_abs();
                    if deviation > SEEK_CORRECTION_THRESHOLD_SECS {
                        let payload = encode_media_metadata_payload(&now, true);
                        spawn_metadata_push(app, peripheral, chr_media, payload).await;
                        np.last_pushed_position = Some((actual, now_epoch_secs()));
                    }
                }
            }

            // Album art: checked independently of whether metadata changed
            // above — never nested inside the `track != np.last_track`
            // branch (that was the bug: a same-track art update, arriving
            // on its own MediaPropertiesChanged ping, was never even looked
            // at). Only run at all when this ping's source could plausibly
            // carry an art change.
            if check_art {
                check_and_push_album_art(app, peripheral, chr_art, np).await;
            }
        }
        Ok(None) if np.last_track != <(String, String, bool)>::default() => {
            abort_album_art_task(app).await;
            np.last_track = Default::default();
            np.last_art_hash = None;
            np.last_pushed_position = None;
            spawn_metadata_push(app, peripheral, chr_media, encode_empty_media_metadata_payload()).await;
        }
        _ => {}
    }
}

/// Fetches the current track's raw OS thumbnail and pushes it to Ori only if
/// its content actually differs from the last thing pushed — a SHA-256 over
/// the raw bytes, not a proxy off title/artist (`NowPlayingState::last_art_hash`'s
/// doc comment explains why the proxy missed real cases). Cheap to call on
/// every qualifying ping: `now_playing_thumbnail()` is a local OS round-trip
/// (no network), and hashing a JPEG/PNG thumbnail (tens of KB) is negligible
/// next to the decode/resize/re-encode work `run_album_art_push` only does
/// once a real change is confirmed.
async fn check_and_push_album_art(
    app: &AppHandle,
    peripheral: &Peripheral,
    chr_art: Option<&Characteristic>,
    np: &mut NowPlayingState,
) {
    let Ok(raw) = media::now_playing_thumbnail().await else { return };
    let hash = if raw.is_empty() { None } else { Some(sha256(&raw)) };
    if hash == np.last_art_hash {
        return;
    }
    np.last_art_hash = hash;
    // Empty thumbnail (source has no art right now) — nothing to send; Ori
    // keeps showing whatever art it last received rather than being told to
    // clear it, matching how "nothing playing" is handled elsewhere.
    if !raw.is_empty() {
        spawn_album_art_push(app, peripheral, chr_art, raw).await;
    }
}

/// Controls-mode OS bridge (§12) — subscribes to KeyboardCommand (char
/// 000A), dispatches each notify to an OS action via `ble::media`, and
/// reacts to real OS push notifications for both volume/mute
/// (`media::watch_volume`, WASAPI's `IAudioEndpointVolumeCallback`) and
/// now-playing state (`media::watch_now_playing`, GSMTC's change events) —
/// falling back to a wide poll for now-playing only if its watcher couldn't
/// be set up. Runs until the link drops — event-driven via
/// `state.disconnect_tx`, not polled; the two inline `is_connected()` checks
/// below are just a cheap belt-and-suspenders guard against writing to a
/// peripheral that dropped in the same `select!` iteration as an
/// already-queued ping, not the actual disconnect-detection mechanism.
async fn media_bridge(app: AppHandle, peripheral: Peripheral) {
    let (Ok(chr_keyboard), Ok(chr_volume), Ok(chr_media)) = (
        find_char(&peripheral, gatt::CHR_KEYBOARD_COMMAND),
        find_char(&peripheral, gatt::CHR_HOST_VOLUME_STATE),
        find_char(&peripheral, gatt::CHR_MEDIA_METADATA),
    ) else {
        return; // Ori didn't advertise the Controls-mode characteristics — nothing to bridge.
    };
    // Album Art is optional — an older Ori without char 000D still gets
    // metadata/volume bridging, just no art pushes.
    let chr_art = find_char(&peripheral, gatt::CHR_MEDIA_ALBUM_ART).ok();
    if peripheral.subscribe(&chr_keyboard).await.is_err() {
        return;
    }
    let Ok(mut notifications) = peripheral.notifications().await else { return };
    // Unused directly here now (spawn_metadata_push/spawn_album_art_push
    // each re-fetch their own state from the AppHandle they're given, same
    // pattern as `run_shortcut`, since a Tauri-managed-state borrow isn't
    // `'static` and those tasks are spawned) — kept for `state.disconnect_tx`
    // below.
    let state = app.state::<BleState>();

    // Reconnect semantics (§12): push the current OS state immediately so
    // Ori isn't left showing a stale swipe-bar level or "nothing playing"
    // when something actually is.
    let mut last_volume = media::get_master_volume_percent().ok();
    if let Some(level) = last_volume {
        let muted = media::is_master_muted().unwrap_or(false);
        let _ = push_host_volume(&peripheral, &chr_volume, level, muted).await;
    }
    let mut np = NowPlayingState::default();
    if let Ok(Some(now)) = media::now_playing().await {
        np.last_track = (now.title.clone(), now.artist.clone(), now.playing);
        np.last_pushed_position = now.position_s.map(|p| (p, now_epoch_secs()));
        let payload = encode_media_metadata_payload(&now, true);
        spawn_metadata_push(&app, &peripheral, &chr_media, payload).await;
        // Seeds `last_art_hash` from whatever art (if any) is available right
        // now and pushes it — same hash-diff path a later ping would take,
        // so there's no separate "always push on connect" special case.
        check_and_push_album_art(&app, &peripheral, chr_art.as_ref(), &mut np).await;
    } else {
        spawn_metadata_push(&app, &peripheral, &chr_media, encode_empty_media_metadata_payload()).await;
    }

    // Event-driven now-playing detection (see `media::watch_now_playing`'s
    // doc comment for why this replaces polling rather than just widening
    // it). `_now_playing_watcher` has no fields read after construction —
    // it's kept alive purely so its `Drop` unregisters the GSMTC
    // subscription when this task ends. If it fails to set up at all (rare
    // — would mean GSMTC itself was unavailable, in which case the initial
    // `now_playing()` fetch above already came back empty/failed too),
    // `now_playing_active` stays false and the volume-poll branch below
    // covers now-playing changes as a (slower) fallback instead.
    let (now_playing_tx, mut now_playing_rx) = tokio::sync::mpsc::unbounded_channel::<media::NowPlayingChange>();
    let _now_playing_watcher = media::watch_now_playing(now_playing_tx).await.ok();
    let mut now_playing_active = _now_playing_watcher.is_some();

    // Event-driven volume/mute detection (WASAPI's own
    // `IAudioEndpointVolumeCallback` — `media::watch_volume`'s doc comment).
    // `_volume_watcher` has no fields read after construction, same
    // kept-alive-for-its-Drop pattern as `_now_playing_watcher` above. If it
    // fails to set up (no volume-capable render endpoint at all — rare),
    // volume changes made outside Ori simply won't be caught until the next
    // reconnect; there's no poll fallback for it the way now-playing has
    // one, since unlike GSMTC there's no separate "poll this instead"
    // primitive to fall back to here — `get_master_volume_percent()` IS the
    // thing `watch_volume` wraps push notifications around.
    let (volume_tx, mut volume_rx) = tokio::sync::mpsc::unbounded_channel::<()>();
    let _volume_watcher = media::watch_volume(volume_tx).await.ok();
    let mut volume_active = _volume_watcher.is_some();

    let mut now_playing_fallback_poll = tokio::time::interval(NOW_PLAYING_FALLBACK_POLL_INTERVAL);
    now_playing_fallback_poll.tick().await; // first tick fires immediately; the pushes above already covered "now"
    let mut disconnect_rx = state.disconnect_tx.subscribe();

    loop {
        tokio::select! {
            _ = disconnect_rx.changed() => return,
            maybe_notif = notifications.next() => {
                let Some(notif) = maybe_notif else { return };
                if notif.uuid != chr_keyboard.uuid {
                    continue;
                }
                let Ok(cmd) = cbor::decode::<cbor::KeyboardCommand>(&notif.value) else { continue };
                handle_keyboard_command(&app, &peripheral, &chr_volume, &cmd).await;
            }
            maybe_ping = now_playing_rx.recv(), if now_playing_active => {
                let Some(mut kind) = maybe_ping else {
                    // Channel closed (the watcher's own registration must have
                    // failed after all) — disable this branch for the rest of
                    // the loop; the fallback-poll branch takes over.
                    now_playing_active = false;
                    continue;
                };
                // A single track change can fire more than one GSMTC event
                // (e.g. MediaPropertiesChanged + PlaybackInfoChanged) —
                // drain any already-queued pings so this does one re-fetch
                // instead of one per event. Widen `kind` to `Properties` if
                // ANY drained ping was one — a coalesced batch must not lose
                // a real art-eligible signal just because a PlaybackInfo/
                // Timeline ping happened to queue behind or ahead of it.
                while let Ok(next) = now_playing_rx.try_recv() {
                    if next == media::NowPlayingChange::Properties {
                        kind = media::NowPlayingChange::Properties;
                    }
                }
                if !peripheral.is_connected().await.unwrap_or(false) {
                    return;
                }
                let check_art = kind == media::NowPlayingChange::Properties;
                check_and_push_now_playing(&app, &peripheral, &chr_media, chr_art.as_ref(), &mut np, check_art).await;
            }
            maybe_vol_ping = volume_rx.recv(), if volume_active => {
                let Some(()) = maybe_vol_ping else {
                    // Channel closed (the watcher's blocking thread ended
                    // early — no volume-capable render endpoint at all).
                    // Nothing to fall back to here (unlike now-playing,
                    // there's no separate poll primitive to catch this) —
                    // disable the branch so this doesn't busy-loop on `None`.
                    volume_active = false;
                    continue;
                };
                if !peripheral.is_connected().await.unwrap_or(false) {
                    return;
                }
                if let Ok(level) = media::get_master_volume_percent() {
                    if Some(level) != last_volume {
                        last_volume = Some(level);
                        let muted = media::is_master_muted().unwrap_or(false);
                        let _ = push_host_volume(&peripheral, &chr_volume, level, muted).await;
                    }
                }
            }
            // Only relevant at all when `watch_now_playing` itself failed to
            // set up — guarded so this doesn't wake for nothing every tick
            // once the real GSMTC-event path is doing its job.
            _ = now_playing_fallback_poll.tick(), if !now_playing_active => {
                if !peripheral.is_connected().await.unwrap_or(false) {
                    return;
                }
                // Blind poll with no event-kind info — always re-check art too.
                check_and_push_now_playing(&app, &peripheral, &chr_media, chr_art.as_ref(), &mut np, true).await;
            }
        }
    }
}

async fn handle_keyboard_command(
    app: &AppHandle,
    peripheral: &Peripheral,
    chr_volume: &Characteristic,
    cmd: &cbor::KeyboardCommand,
) {
    match cmd.o.as_str() {
        "play_pause" => media::play_pause(),
        "next" => media::next(),
        "prev" => media::prev(),
        "seek" => {
            let _ = media::seek(cmd.a).await;
        }
        "vol_set" => {
            let level = cmd.a.min(100) as u8;
            if media::set_master_volume_percent(level).is_ok() {
                let muted = media::is_master_muted().unwrap_or(false);
                let _ = push_host_volume(peripheral, chr_volume, level, muted).await;
            }
        }
        "shortcut" => run_shortcut(app, cmd.a).await,
        _ => {}
    }
}

/// Looks up the token last written to slot `slot` (1..3) — session memory
/// only, see `BleState::shortcut_slots` — and runs its action (§12).
async fn run_shortcut(app: &AppHandle, slot: u32) {
    let Some(index) = (slot as usize).checked_sub(1) else { return };
    let state = app.state::<BleState>();
    let token = {
        let slots = state.shortcut_slots.lock().await;
        slots.get(index).cloned().unwrap_or_default()
    };
    match token.as_str() {
        "vol-mute" => {
            let _ = media::toggle_master_mute();
        }
        "mic-mute" => {
            let _ = media::toggle_mic_mute();
        }
        "screenshot" => media::trigger_screenshot(),
        "lock-screen" => {
            let _ = media::lock_screen();
        }
        "calculator" => {
            let _ = media::open_calculator();
        }
        // Standard OS edit shortcuts — replayed via the same `press_combo`
        // primitive the recorded Favorite combos use, just with a fixed
        // combo instead of a user-recorded one (media-mode.md).
        "copy" => {
            let _ = media::press_combo(&["Ctrl".to_string(), "C".to_string()]);
        }
        "cut" => {
            let _ = media::press_combo(&["Ctrl".to_string(), "X".to_string()]);
        }
        "paste" => {
            let _ = media::press_combo(&["Ctrl".to_string(), "V".to_string()]);
        }
        "undo" => {
            let _ = media::press_combo(&["Ctrl".to_string(), "Z".to_string()]);
        }
        // Ctrl+Y is the standard Windows redo shortcut (Word, Excel, most
        // Windows apps) — Ctrl+Shift+Z is the macOS/browser convention, not
        // used here since this dispatch path is Windows-only today.
        "redo" => {
            let _ = media::press_combo(&["Ctrl".to_string(), "Y".to_string()]);
        }
        "save" => {
            let _ = media::press_combo(&["Ctrl".to_string(), "S".to_string()]);
        }
        // Favorite 1/2/3 (media-mode.md) are three independent tokens, but the
        // recorded combo is keyed by SLOT (favorite_combos[index]), not by
        // which favorite-N is assigned there — matches the frontend's design
        // (app.js's per-slot _kbdCombos), so any "favorite-*" token reads the
        // same slot's combo regardless of its number suffix.
        t if t.starts_with("favorite") => {
            let combo = {
                let combos = state.favorite_combos.lock().await;
                combos.get(index).cloned().unwrap_or_default()
            };
            if !combo.is_empty() {
                let _ = media::press_combo(&combo);
            }
        }
        // Unrecognized tokens are no-ops.
        _ => {}
    }
}

/// Updates the session cache `run_shortcut`'s "favorite" dispatch reads
/// from (see `BleState::favorite_combos`'s doc comment). Called both when
/// the user records a new combo and at app startup to seed it from
/// persisted storage.
pub async fn set_favorite_combos(state: &BleState, combos: [Vec<String>; 3]) {
    *state.favorite_combos.lock().await = combos;
}

async fn push_host_volume(peripheral: &Peripheral, chr: &Characteristic, level: u8, muted: bool) -> Result<(), String> {
    let payload = cbor::encode(&cbor::HostVolumeState { l: level, m: muted });
    peripheral.write(chr, &payload, WriteType::WithResponse).await.map_err(|e| e.to_string())
}

/// ble-protocol.md §10: `MediaMetadata.title` ≤ 192 UTF-8 bytes, `.artist` ≤
/// 96. Unlike Profile Photo / Meeting List / Time Off / Album Art (all
/// chunked, §5), this characteristic is a single non-chunked write — an
/// oversized payload can fail outright on a constrained MTU instead of just
/// being reassembled fragment-by-fragment, so both fields are truncated
/// right here at the one push site, covering every caller regardless of
/// where the title/artist string originated.
const MEDIA_TITLE_MAX_BYTES: usize = 192;
const MEDIA_ARTIST_MAX_BYTES: usize = 96;

/// Truncates `s` to at most `max_bytes` UTF-8 bytes without splitting a
/// multi-byte character — walks back from `max_bytes` to the nearest
/// preceding UTF-8 character boundary (`str::is_char_boundary`), so the
/// result is always valid UTF-8 even if that lands mid-character.
fn truncate_utf8(s: &str, max_bytes: usize) -> &str {
    if s.len() <= max_bytes {
        return s;
    }
    let mut end = max_bytes;
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

fn encode_media_metadata_payload(now: &media::NowPlaying, include_position: bool) -> Vec<u8> {
    let title = truncate_utf8(&now.title, MEDIA_TITLE_MAX_BYTES);
    let artist = truncate_utf8(&now.artist, MEDIA_ARTIST_MAX_BYTES);
    cbor::encode(&cbor::MediaMetadata {
        t: title,
        a: artist,
        c: now.can_seek,
        p: now.playing,
        o: if include_position { now.position_s } else { None },
        d: if include_position { now.duration_s } else { None },
    })
}

fn encode_empty_media_metadata_payload() -> Vec<u8> {
    cbor::encode(&cbor::MediaMetadata { t: "", a: "", ..Default::default() })
}

/// Submits a MediaMetadata write, coalescing with any already-outstanding
/// one instead of running them concurrently or queuing them up. See
/// `BleState::media_meta_push`'s doc comment for WHY: char 000C is Write-
/// WITH-Response only, and the BLE ATT protocol permits just one such
/// request outstanding per connection — a `tokio::task::abort()` on the
/// SENDING task (what album art's mechanism does, and what an earlier
/// version of this function also tried) cannot retract an already-
/// dispatched Write Request, so aborting-and-restarting on every rapid
/// track change just left a growing backlog of stale, un-cancellable
/// requests that Ori had to ack one by one before the LAST (wanted) one's
/// turn came up — observed on hardware taking over a second.
///
/// The fix: never have more than one Write Request in flight. If nothing is
/// currently outstanding, this spawns a worker that sends `payload` and
/// then keeps looping — each iteration checking `pending` again — until
/// there's genuinely nothing newer left, at which point it marks itself not-
/// running and exits. If a write IS already outstanding, this just replaces
/// `pending` with `payload` (discarding whatever was queued there before,
/// since only the LATEST track's metadata is ever worth sending) and
/// returns immediately — the already-running worker will pick it up the
/// moment its current ack arrives. `running`/`pending`/the spawn decision
/// are all made under one lock acquisition so there's no window where a
/// worker that's about to exit can miss a `pending` value set moments
/// earlier (see `MediaMetaPushState`'s own doc comment).
async fn spawn_metadata_push(app: &AppHandle, peripheral: &Peripheral, chr_media: &Characteristic, payload: Vec<u8>) {
    let state = app.state::<BleState>();
    let mut mp = state.media_meta_push.lock().await;
    mp.pending = Some(payload);
    if mp.running {
        return;
    }
    mp.running = true;
    drop(mp);

    let app = app.clone();
    let peripheral = peripheral.clone();
    let chr_media = chr_media.clone();
    let handle = tokio::spawn(async move {
        let state = app.state::<BleState>();
        loop {
            let next = {
                let mut mp = state.media_meta_push.lock().await;
                match mp.pending.take() {
                    Some(payload) => Some(payload),
                    None => {
                        mp.running = false;
                        None
                    }
                }
            };
            let Some(payload) = next else { break };
            let write = peripheral.write(&chr_media, &payload, WriteType::WithResponse);
            let _ = tokio::time::timeout(METADATA_WRITE_TIMEOUT, write).await;
        }
    });
    state.media_meta_push.lock().await.task = Some(handle);
}

/// Cancels whatever album-art transfer is currently in flight, without
/// starting a replacement. Split out from `spawn_album_art_push` so
/// `check_and_push_now_playing` can call this THE INSTANT it detects a real
/// track-identity change (title/artist differ — a new piece of content,
/// full stop), rather than only as a side effect of deciding to send new
/// art. That distinction matters: deciding whether there's new art to send
/// requires fetching and hashing the new track's OS thumbnail first
/// (`check_and_push_album_art`), and leaving the OLD transfer running for
/// that entire round trip gave it enough time, in practice, to finish
/// sending on its own — a typical art payload (15-30 KB target, §10) can
/// complete in well under a second, often faster than "detect track change
/// -> fetch new thumbnail -> hash it" takes. Calling this first closes that
/// window: the stale transfer is cut off immediately, before any of that
/// slower work even starts. A no-op if nothing is in flight.
async fn abort_album_art_task(app: &AppHandle) {
    let state = app.state::<BleState>();
    let mut slot = state.album_art_task.lock().await;
    if let Some(prev) = slot.take() {
        prev.abort();
    }
}

/// Requests an album-art push for the current track, aborting any still-in-
/// flight previous art transfer first (belt-and-suspenders — the caller has
/// normally already done this via `abort_album_art_task` the moment a track
/// change was detected, well before this function's own OS thumbnail
/// fetch+hash decided there was new art to send) so a rapid track change
/// can never queue behind a stale transfer streaming to completion. Unlike
/// MediaMetadata (`spawn_metadata_push`), Album Art (char 000D) is
/// Write-No-Response only — no ATT-level outstanding-request limit — so a
/// `tokio::task::abort()` on the sending task genuinely does stop further
/// fragments going out. Ori also independently discards a half-sent old
/// image the moment the new transfer's seq-0 frame arrives (firmware
/// `chunked_transfer.cpp` resets on seq 0). A no-op if Ori didn't advertise
/// the Album Art characteristic. `raw` is the OS thumbnail already fetched
/// by the caller (`check_and_push_album_art`) to compute its content hash —
/// passed through rather than re-fetched here so a confirmed art change
/// costs exactly one OS round-trip, not two.
async fn spawn_album_art_push(app: &AppHandle, peripheral: &Peripheral, chr_art: Option<&Characteristic>, raw: Vec<u8>) {
    let Some(chr_art) = chr_art else { return };
    abort_album_art_task(app).await;
    let state = app.state::<BleState>();
    let mut slot = state.album_art_task.lock().await;
    let app = app.clone();
    let peripheral = peripheral.clone();
    let chr_art = chr_art.clone();
    *slot = Some(tokio::spawn(async move {
        let state = app.state::<BleState>();
        run_album_art_push(&state, &peripheral, &chr_art, raw).await;
    }));
}

/// Builds Ori's 484×216 JPEG from `raw` (the OS thumbnail bytes already
/// fetched and hash-checked by `check_and_push_album_art`) and pushes it
/// chunked (Write-No-Response only, §5/§12). Silently does nothing if it
/// fails to decode — art is a nice-to-have, never worth failing the
/// track-change push over. Runs as its own task (`spawn_album_art_push`);
/// may be aborted mid-stream by a newer track's art request.
async fn run_album_art_push(state: &BleState, peripheral: &Peripheral, chr_art: &Characteristic, raw: Vec<u8>) {
    // Decode/resize/encode is CPU-bound (up to several JPEG re-encodes, see
    // `build_album_art_jpeg`'s doc comment) — run it on the blocking pool
    // rather than directly on this Tokio worker thread, which the phone-bond
    // watcher, time-sync refresher, and keyboard-command dispatch for this
    // same connection all share.
    let Ok(jpeg) = tokio::task::spawn_blocking(move || build_album_art_jpeg(&raw)).await else { return };
    if jpeg.is_empty() {
        return;
    }

    // Let any outstanding MediaMetadata write drain first. Metadata (Write-
    // WITH-Response) and this art transfer (Write-No-Response) use separate
    // ATT mechanisms and don't block each other at the protocol level, but
    // they still share ONE physical BLE connection — on hardware, firing
    // both at once measurably let them contend for the same airtime: a
    // capture showed a metadata ack taking 2.18s at the same time this
    // function's own write phase took ~2.0s, for what's normally a ~50ms /
    // ~150ms operation respectively. Waiting here costs nothing in the
    // common case (metadata, at ~30-60ms typically, usually finishes well
    // before this function reaches this point, most of which was spent on
    // the JPEG re-encode above plus whatever the caller's own OS thumbnail
    // fetch took) and bounds the worst case (a stuck/slow metadata ack) so
    // this never waits forever.
    let wait_start = std::time::Instant::now();
    loop {
        if !state.media_meta_push.lock().await.running {
            break;
        }
        if wait_start.elapsed() > ART_METADATA_WAIT_TIMEOUT {
            break;
        }
        tokio::time::sleep(Duration::from_millis(5)).await;
    }

    // Serializes against every other chunked/BEGIN-END write (see
    // `BleState::sync_lock`'s doc comment). Needed here specifically because
    // a chunk gap/timeout NACK (§5/§8) carries no characteristic identifier —
    // two chunk streams in flight at once (e.g. this album-art push racing a
    // staged profile photo from a concurrent Save Profile) would make a NACK
    // ambiguous, since `write_chunked`'s NACK-drain for the *other* stream
    // could wrongly attribute a NACK that was actually about this one. Album
    // art is small (≤ 64 KB) so briefly holding the lock here is harmless.
    // (sync_lock acquisition itself has been confirmed on hardware to always
    // be near-instant, single-digit microseconds — not a bottleneck.)
    let _sync_guard = state.sync_lock.lock().await;
    let _ = write_chunked_no_response(peripheral, chr_art, &jpeg).await;
}

#[derive(Serialize, Clone)]
struct FwUpdateAvailableEvent {
    version: &'static str,
}

/// Latest known firmware version — until Phase C's real `ori.app` polling
/// lands (ota.md), this is a hand-maintained stand-in for "the newest
/// release." Update this constant when a new firmware version ships.
const LATEST_FIRMWARE_VERSION: &str = "1.0.0";

/// One-shot read of the Firmware Revision String (Device Information
/// Service, `0x2A26` — a separate BLE SIG standard service from the Ori
/// Sync Service, unencrypted, plain UTF-8 semver — ble-protocol.md §3.1/§9).
/// Emits `fw-update-available` if Ori's running version is older than
/// `LATEST_FIRMWARE_VERSION`, matching the frontend's existing
/// `listen('fw-update-available', ...)` hook.
async fn check_firmware_version(app: AppHandle, peripheral: Peripheral) {
    let Ok(chr) = find_char(&peripheral, gatt::CHR_FW_REVISION) else { return };
    let Ok(raw) = peripheral.read(&chr).await else { return };
    let Ok(version) = String::from_utf8(raw) else { return };
    let version = version.trim().to_string();
    // Cached for the Ori Info/Stats modal (`get_ori_info`) — this is the
    // only place that ever reads the Firmware Revision String characteristic.
    *app.state::<BleState>().cached_fw_version.lock().await = Some(version.clone());
    if is_older_version(&version, LATEST_FIRMWARE_VERSION) {
        let _ = app.emit("fw-update-available", FwUpdateAvailableEvent { version: LATEST_FIRMWARE_VERSION });
    }
}

/// Numeric major.minor.patch comparison — semver-ish, not a full parser;
/// enough for the plain "1.0.0"-style versions ble-protocol.md §9 specifies.
/// Missing or non-numeric parts compare as 0.
fn is_older_version(current: &str, latest: &str) -> bool {
    fn parts(v: &str) -> [u32; 3] {
        let mut out = [0u32; 3];
        for (i, p) in v.split('.').take(3).enumerate() {
            out[i] = p.parse().unwrap_or(0);
        }
        out
    }
    parts(current) < parts(latest)
}

/// Write-through caches `status.d` (device_type) the same way
/// `read_device_settings` caches serial_number/manufacture_date — session
/// (`BleState::cached_phone_device_type`) + disk (`store::SavedState`) — but
/// only on an actual non-empty value and an actual change, so this isn't a
/// disk write on every routine stats notify (queue changes, RSSI polls) once
/// the value is already known. A disconnect/unpair pushes `d:""`, which this
/// deliberately ignores — `store::SavedState::phone_device_type`'s own doc
/// comment covers who's responsible for clearing it (Factory Reset/Clear
/// All/`give_up_on_bond`/`unpair_phone`), not a live "" from the wire.
async fn maybe_persist_phone_device_type(app: &AppHandle, status: &cbor::PhoneBondStatus) {
    if status.d.is_empty() {
        return;
    }
    let state = app.state::<BleState>();
    let mut cached = state.cached_phone_device_type.lock().await;
    if cached.as_deref() == Some(status.d.as_str()) {
        return;
    }
    *cached = Some(status.d.clone());
    drop(cached);
    if let Some(mut saved) = crate::store::load(app).await {
        saved.phone_device_type = Some(status.d.clone());
        let _ = crate::store::save(app, &saved).await;
    }
}

/// Tracks Ori's iPhone ANCS bond/connection state (char `000F`) for the
/// lifetime of the connection — reads the current value once up front (so
/// Orion's UI doesn't have to wait for the next change to learn it), then
/// relays every subsequent notify, both as a `phone-bond-status` event
/// (ble-protocol.md §3/§11).
async fn phone_bond_watcher(app: AppHandle, peripheral: Peripheral) {
    let Ok(chr) = find_char(&peripheral, gatt::CHR_PHONE_BOND_STATUS) else { return };
    if peripheral.subscribe(&chr).await.is_err() {
        return;
    }
    if let Ok(raw) = peripheral.read(&chr).await {
        if let Ok(status) = cbor::decode::<cbor::PhoneBondStatus>(&raw) {
            maybe_persist_phone_device_type(&app, &status).await;
            let _ = app.emit("phone-bond-status", status);
        }
    }
    let Ok(mut notifications) = peripheral.notifications().await else { return };
    let mut disconnect_rx = app.state::<BleState>().disconnect_tx.subscribe();
    loop {
        tokio::select! {
            maybe_notif = notifications.next() => {
                let Some(notif) = maybe_notif else { return };
                if notif.uuid != chr.uuid {
                    continue;
                }
                if let Ok(status) = cbor::decode::<cbor::PhoneBondStatus>(&notif.value) {
                    maybe_persist_phone_device_type(&app, &status).await;
                    let _ = app.emit("phone-bond-status", status);
                }
            }
            _ = disconnect_rx.changed() => return,
        }
    }
}

/// Relays Device Settings (char `000E`) notifies as `device-settings-update`
/// events. Ori now pushes a fresh notify whenever its own live `signal_bars`
/// ("r") bucket to Orion changes — the one genuinely live field in this
/// characteristic; the NVS-persisted fields (clock_face/time_format/
/// ancs_filter/shortcuts) only ever change via an Orion write in the first
/// place, so Orion already knows their new value the moment it sends one and
/// has no need to be told again. This is what lets the frontend's Ori Info
/// modal drop its old fixed-interval poll (pc-app.md) — see app.js's
/// `listen('device-settings-update', ...)`. No up-front read here (unlike
/// `phone_bond_watcher`'s): `read_device_settings` already covers the
/// (re)connect case on its own (app.js's `readSlotsFromDevice()`).
async fn device_settings_watcher(app: AppHandle, peripheral: Peripheral) {
    let Ok(chr) = find_char(&peripheral, gatt::CHR_DEVICE_SETTINGS) else { return };
    if peripheral.subscribe(&chr).await.is_err() {
        return;
    }
    let Ok(mut notifications) = peripheral.notifications().await else { return };
    let mut disconnect_rx = app.state::<BleState>().disconnect_tx.subscribe();
    loop {
        tokio::select! {
            maybe_notif = notifications.next() => {
                let Some(notif) = maybe_notif else { return };
                if notif.uuid != chr.uuid {
                    continue;
                }
                if let Ok(settings) = cbor::decode::<cbor::DeviceSettingsRead>(&notif.value) {
                    let _ = app.emit("device-settings-update", settings);
                }
            }
            _ = disconnect_rx.changed() => return,
        }
    }
}

/// Pre-subscribes to the ANCS relay characteristics (chars 0010/0011,
/// ble-protocol.md §13) and creates their notification receivers as early as
/// possible — right after the link is confirmed encrypted, BEFORE
/// `run_sync`'s slower Device Settings/Manifest/BEGIN-END exchange even
/// starts. Fixes a race specific to BONDED RECONNECTS (found from a 2026-07
/// hardware log: `PhoneBondStatus` count correct, e.g. "3 missed calls," but
/// the drill-down list for every bucket empty — reproducible on reconnect,
/// never on first pairing):
///
/// NimBLE persists each peer's CCCD subscription state (`firmware.md`'s
/// "NimBLE persisted-CCCD store" note) and fires `onSubscribe()` — which
/// triggers Ori's `resync_orion_relay()`/`resync_orion_call_state()`,
/// replaying the peer's WHOLE current ANCS queue/call state — the MOMENT the
/// encrypted link comes back up on a bonded reconnect, entirely independent
/// of when Orion's own Rust code gets around to subscribing again. On a
/// first pairing there's no persisted subscription for NimBLE to
/// auto-restore, so Ori's resync only ever fires in response to Orion's OWN
/// (later) subscribe() call — by which point `ancs_notification_watcher`'s
/// old in-function "receiver before subscribe" ordering already guaranteed a
/// receiver existed. On reconnect, though, that persisted-CCCD resync can —
/// and, per the log, does — arrive while Orion is still deep inside
/// `run_sync`, well before `start_post_sync_tasks` has even spawned the
/// watcher tasks that used to be what created these receivers. A
/// `tokio::broadcast` receiver only sees messages sent AFTER it exists, so
/// with none alive yet, that resync burst is silently and permanently lost:
/// the aggregate count (char 000F, read fresh later by `phone_bond_watcher`)
/// still ends up correct, but the underlying notification list — and any
/// already-ringing/active call — ends up empty for the rest of that
/// connection, since the resync is a one-shot event Ori never repeats.
///
/// **A receiver existing isn't enough — it also has to be DRAINED early.**
/// `peripheral.notifications()` on btleplug's Windows backend hands back a
/// receiver onto ONE broadcast channel SHARED across every characteristic on
/// this peripheral (every notify lands in the same stream, not just char
/// 0010's), with a small fixed-capacity ring buffer (~16 slots). The previous
/// version of this function created the two receivers here but left them
/// undrained until `start_post_sync_tasks` spawned `ancs_notification_watcher`/
/// `ancs_call_state_watcher` — which only happens AFTER `run_sync` fully
/// completes. In between, `run_sync` alone generates well over 16 notifies on
/// the SAME shared channel — Device Status (RECONNECTING/SYNCING/READY), Sync
/// Manifest, Phone Bond Status, Sync Control ACKs — none of which either
/// receiver is reading yet. That overflows the ring and silently evicts the
/// oldest entries, which is exactly the ANCS resync burst(s) this function
/// exists to catch: reproduced on hardware 2026-07-12, where the char 0010
/// resync payload never arrived even though Ori's own log confirmed it sent
/// "clear" + every queued item. This cost firmware's OWN guaranteed-late
/// second resync (`state_machine::on_reconnect_end()`, `firmware.md`) its
/// entire reason to exist: that resync is causally guaranteed to land after
/// Orion subscribes, but not after Orion actually STARTS READING — the two
/// used to be the same moment, back when the watcher tasks were spawned
/// synchronously right after subscribing; deferring them to
/// `start_post_sync_tasks` broke that equivalence.
///
/// Fix: spawn the drain tasks HERE, immediately, instead of just creating and
/// returning the streams for someone else to drain later. `ancs_tasks`
/// (separate from `background_tasks`) tracks them so `start_post_sync_tasks`
/// — which unconditionally aborts and respawns everything in
/// `background_tasks` on every (re)connect — can't kill these moments after
/// they start; a fast repeated reconnect instead aborts the PREVIOUS pair
/// stored here before spawning this connection's replacements, mirroring
/// `start_post_sync_tasks`'s own stale-task cleanup for the same reason
/// (see `background_tasks`'s doc comment).
async fn subscribe_ancs_relay_early(app: &AppHandle, state: &BleState, peripheral: &Peripheral) -> Result<(), String> {
    let chr_notif = find_char(peripheral, gatt::CHR_ANCS_NOTIFICATION)?;
    let chr_call = find_char(peripheral, gatt::CHR_ANCS_CALL_STATE)?;
    let notif_stream: NotifyStream = peripheral.notifications().await.map_err(|e| e.to_string())?;
    let call_stream: NotifyStream = peripheral.notifications().await.map_err(|e| e.to_string())?;
    peripheral.subscribe(&chr_notif).await.map_err(|e| e.to_string())?;
    peripheral.subscribe(&chr_call).await.map_err(|e| e.to_string())?;

    // Earliest point after the link is confirmed encrypted, common to both
    // `reconnect` and `submit_passkey` — the natural single choke point to
    // arm the event-driven disconnect signal every watcher below (and
    // `wait_for_disconnect`) subscribes to instead of polling.
    let adapter = get_adapter(state).await?;
    spawn_disconnect_watcher(state, &adapter, peripheral).await;

    let mut tasks = state.ancs_tasks.lock().await;
    for task in tasks.drain(..) {
        task.abort();
    }
    tasks.push(tokio::spawn(ancs_notification_watcher(app.clone(), peripheral.clone(), notif_stream)));
    tasks.push(tokio::spawn(ancs_call_state_watcher(app.clone(), peripheral.clone(), call_stream)));
    Ok(())
}

/// Relays Ori's per-notification ANCS content (char 0010, ble-protocol.md
/// §13) to the frontend as `ancs-notification` events. Unlike Phone Bond
/// Status this characteristic is Notify-only (no Read property, §3's table)
/// — there's nothing to fetch up front on connect, only notifies to relay
/// from here on. Filtering is entirely Ori-side (`ancs_filter`, Device
/// Settings `"f"`): Orion never re-filters what arrives, it just relays it
/// faithfully — including `"clear"` (sent when the user changes the filter,
/// followed by `"add"` for everything that now passes) so the frontend's
/// local mirror stays in lockstep with Ori's. Runs until the link drops —
/// event-driven via `state.disconnect_tx`, not polled (`watch_for_disconnect`'s
/// doc comment).
///
/// Spawned by `subscribe_ancs_relay_early` itself, immediately after
/// creating and subscribing `notifications` — see its doc comment for why
/// draining has to start that early rather than being deferred to
/// `start_post_sync_tasks` (the shared-notify-buffer overflow that used to
/// silently drop this exact stream).
async fn ancs_notification_watcher(app: AppHandle, peripheral: Peripheral, mut notifications: NotifyStream) {
    let Ok(chr) = find_char(&peripheral, gatt::CHR_ANCS_NOTIFICATION) else {
        return;
    };
    // Every notify on this characteristic is chunk-framed now (ble-protocol.md
    // §5's "AncsNotification chunking") — "remove"/"clear"/most "add"s
    // reassemble in one `feed()` call (total_frags:1), a maxed-out "add"'s
    // 512-byte body needs a few. One reassembler for this watcher's whole
    // connection lifetime — a fresh connection gets a fresh instance, so
    // there's no cross-connection state to worry about.
    let mut chunks = chunk::Reassembler::new();
    let mut disconnect_rx = app.state::<BleState>().disconnect_tx.subscribe();
    loop {
        tokio::select! {
            maybe_notif = notifications.next() => {
                let Some(notif) = maybe_notif else {
                    return;
                };
                if notif.uuid != chr.uuid {
                    continue;  // not char 0010 — another characteristic's notify on the shared stream
                }
                let Some(complete) = chunks.feed(&notif.value) else {
                    continue;  // mid-sequence fragment, or a dropped/malformed frame — chunk::Reassembler's own doc comment covers why this self-heals without a NACK
                };
                if let Ok(payload) = cbor::decode::<cbor::AncsNotification>(&complete) {
                    let _ = app.emit("ancs-notification", payload);
                }
            }
            _ = disconnect_rx.changed() => return,
        }
    }
}

/// Relays Ori's live call state (char 0011, notify-only, ble-protocol.md
/// §13) to the frontend as `ancs-call-state` events. On a ringing
/// transition (`st == 1`) brings the Orion window to the foreground BEFORE
/// emitting the event — "Call takeover": orion-sync owns raising the
/// window, orion-frontend owns the in-app UI that follows it (§13/§11).
/// Never fires when `ancs_filter` is Disabled — Ori simply never sends this
/// notify in that case, so no filter check is needed here. Runs until the
/// link drops — event-driven, same as `ancs_notification_watcher`.
///
/// Spawned by `subscribe_ancs_relay_early` itself, immediately after
/// creating and subscribing `notifications` — see its doc comment for why
/// draining has to start that early (the reconnect-only resync race that
/// would otherwise drop an already-ringing/active call, plus the
/// shared-notify-buffer overflow that made "receiver exists early" alone
/// insufficient).
async fn ancs_call_state_watcher(app: AppHandle, peripheral: Peripheral, mut notifications: NotifyStream) {
    let Ok(chr) = find_char(&peripheral, gatt::CHR_ANCS_CALL_STATE) else { return };
    let mut disconnect_rx = app.state::<BleState>().disconnect_tx.subscribe();
    loop {
        tokio::select! {
            maybe_notif = notifications.next() => {
                let Some(notif) = maybe_notif else { return };
                if notif.uuid != chr.uuid {
                    continue;
                }
                if let Ok(payload) = cbor::decode::<cbor::AncsCallState>(&notif.value) {
                    if payload.st == 1 {
                        crate::show_and_focus_panel(&app);
                    }
                    let _ = app.emit("ancs-call-state", payload);
                }
            }
            _ = disconnect_rx.changed() => return,
        }
    }
}

/// Blocks until the currently-connected peripheral (if any) genuinely
/// disconnects — event-driven via `state.disconnect_tx`
/// (`watch_for_disconnect`'s doc comment), not polled. Used by the
/// connection supervisor (`commands.rs`) to know when to start retrying.
/// Returns immediately if there's no active peripheral to watch.
///
/// The upfront `is_connected()` check closes a startup race a bare
/// `disconnect_rx.changed().await` would otherwise have: a fresh
/// `watch::Receiver`'s baseline is whatever `disconnect_tx`'s value already
/// is the moment it subscribes, so if the disconnect (and its bump) already
/// happened a moment before this function got here, waiting on `.changed()`
/// alone would wait for the *next* one forever, even though the peripheral
/// is already gone. `is_connected()` reads the same underlying flag WinRT's
/// `ConnectionStatusChanged` callback sets alongside emitting the event
/// (`btleplug::winrtble::peripheral`) — a cheap local read, not a BLE
/// round-trip — so checking it first, then falling back to the event for
/// everything after, is correct with no polling loop.
pub async fn wait_for_disconnect(state: &BleState) {
    let Some(peripheral) = state.peripheral.lock().await.clone() else { return };
    let mut disconnect_rx = state.disconnect_tx.subscribe();
    loop {
        if !peripheral.is_connected().await.unwrap_or(false) {
            return;
        }
        if disconnect_rx.changed().await.is_err() {
            return; // sender dropped — shouldn't happen, but don't hang forever
        }
    }
}

/// Writes the Factory Reset Device Command (§3/§7.2) — Ori wipes its own
/// NVS + bonds and reboots. A write failure here (including one caused by
/// Ori disconnecting mid-ack, since it can start rebooting before the
/// response arrives) isn't treated as fatal — the magic bytes have very
/// likely already landed either way, matching how
/// `tools/mock_orion_ble.py`'s `run_factory_reset()` treats the same race.
pub async fn factory_reset(state: &BleState) -> Result<(), String> {
    let peripheral = require_peripheral(state).await?;
    let chr = find_char(&peripheral, gatt::CHR_DEVICE_COMMAND)?;
    let _ = peripheral.write(&chr, &gatt::FACTORY_RESET_MAGIC, WriteType::WithResponse).await;

    // Ori is about to wipe its own bond + NVS and reboot (ble-protocol.md
    // §7.2) — our own Windows-level bond record would otherwise go stale
    // and make the next pairing attempt skip the passkey ceremony entirely
    // (`pairing::pair_with_passkey_blocking`'s `IsPaired()` short-circuit),
    // silently failing to re-encrypt against an LTK Ori no longer has.
    let address: u64 = peripheral.address().into();
    let _ = pairing::unpair_device(address).await;
    Ok(())
}

/// Unpair Phone (ble-protocol.md §3/§7): wipes just the iPhone ANCS bond on
/// Ori, reopening the iPhone slot without touching Orion's own PC bond. Ori
/// notifies Phone Bond Status ({b:false, c:false}) once it processes this —
/// `phone_bond_watcher` already picks that up and emits it to the frontend.
pub async fn unpair_phone(state: &BleState) -> Result<(), String> {
    let peripheral = require_peripheral(state).await?;
    let chr = find_char(&peripheral, gatt::CHR_DEVICE_COMMAND)?;
    peripheral.write(&chr, &gatt::UNPAIR_PHONE_MAGIC, WriteType::WithResponse).await.map_err(|e| e.to_string())
}

/// Writes ANCS Notification Action (char 0012, ble-protocol.md §13
/// "Actions") — Answer/Decline/End-call/Dismiss/Read-all, one write per
/// target uid. Deliberately has NO side effect on any local cache: Orion
/// does not update its own UI optimistically here — it waits for the
/// resulting `AncsNotification{op:"remove"}` / `AncsCallState` transition
/// that `ancs_notification_watcher`/`ancs_call_state_watcher` will relay,
/// same as every other state change in this protocol (§6 never assumes a
/// write succeeded before the corresponding notify confirms it).
pub async fn ancs_notification_action(state: &BleState, uid: u32, action: u8) -> Result<(), String> {
    let peripheral = require_peripheral(state).await?;
    let chr = find_char(&peripheral, gatt::CHR_ANCS_NOTIFICATION_ACTION)?;
    peripheral
        .write(&chr, &cbor::encode(&cbor::AncsNotificationAction { u: uid, a: action }), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())
}

// Unified sync flow — ble-protocol.md §6.1 + §6.2 in one implementation,
// mirroring tools/mock_orion_ble.py's run_sync(). The doc's §6.1 sequence
// diagram shows a first pair skipping the manifest round-trip and sending
// everything unconditionally, but that's the degenerate case of §6.2's
// hash-compare mechanism: Ori's NVS is empty, so it always replies
// needed=[everything]. One code path — always send the manifest, always act
// on what Ori says it needs — produces identical wire behavior on a first
// pair and is what the mock (validated against real hardware, per
// CLAUDE.md) does. Two divergent implementations of the same mechanism
// would be a bug waiting to happen, not extra correctness.
//
// `time_off` MUST be whatever Orion last actually synced (or the zeroed
// "nothing set" default on a genuine first pair) — same hazard as
// `reconnect`'s doc comment on `profile`: this function hashes whatever
// it's handed and asks Ori "do you already have this." A reconnect that
// passed a blank Time Off here would make Ori report it as "needed" and
// get it overwritten with empty fields, silently erasing a real entry.
//
// `initial_pair` only steers which frontend event `emit_progress` uses
// (`sync-progress` vs `resync-progress`) — the wire behavior is identical
// either way, per this function's own doc comment above. The frontend's
// `sync-progress` listener drives the *setup wizard's* pairing-phase-3 ring
// and, on completion, calls `suFinishSetup()` — which copies the wizard's
// own (blank, outside a genuine first pair) name/title/email/phone inputs
// into the main screen, overwriting whatever `hydrateProfileCard` had
// already shown. `submit_passkey` (the real first pair) is the only caller
// that should ever trigger that. `reconnect` used to pass through the same
// event, so completing an ordinary background reconnect silently blanked
// the profile card it had just correctly hydrated.
async fn run_sync(
    app: &AppHandle,
    state: &BleState,
    peripheral: &Peripheral,
    profile: &ProfileInput,
    time_off: &TimeOffInput,
    initial_pair: bool,
) -> Result<(), String> {
    // This is the one genuinely large transfer (potentially a profile photo
    // and/or Time Off image on top of the meeting list) — worth the short
    // connection interval for the duration. Settled back to PowerOptimized
    // once the connection reaches steady state (see the callers of
    // `start_post_sync_tasks`).
    set_connection_priority(peripheral, ConnectionParameterPreset::ThroughputOptimized).await;

    let chr_status = find_char(peripheral, gatt::CHR_DEVICE_STATUS)?;
    let chr_time = find_char(peripheral, gatt::CHR_TIME_SYNC)?;
    let chr_profile = find_char(peripheral, gatt::CHR_PROFILE_INFO)?;
    let chr_photo = find_char(peripheral, gatt::CHR_PROFILE_PHOTO)?;
    let chr_meetings = find_char(peripheral, gatt::CHR_MEETING_LIST)?;
    let chr_timeoff = find_char(peripheral, gatt::CHR_TIME_OFF_ENTRY)?;
    let chr_syncctl = find_char(peripheral, gatt::CHR_SYNC_CONTROL)?;
    let chr_manifest = find_char(peripheral, gatt::CHR_SYNC_MANIFEST)?;

    // Serializes against any other mid-session push (see `BleState::sync_lock`'s
    // doc comment) — held for the whole BEGIN..END span below.
    let _sync_guard = state.sync_lock.lock().await;

    for c in [&chr_status, &chr_syncctl, &chr_manifest] {
        peripheral.subscribe(c).await.map_err(|e| e.to_string())?;
    }
    let mut notifications: NotifyStream = peripheral.notifications().await.map_err(|e| e.to_string())?;
    emit_progress(app, 5.0, false, initial_pair);

    // Shortcuts — outside the BEGIN/END pipeline, applied immediately on Ori
    // (§6.1/§6.4). Sent unconditionally on every sync per §6.3 ("Shortcuts —
    // always"), but with whatever Orion's session cache actually holds
    // (`BleState::shortcut_slots`, seeded from persisted storage on
    // reconnect and updated on every user change) — NOT a hardcoded
    // literal, or every sync would silently reset the user's chosen icons
    // back to the firmware default.
    let shortcuts = state.shortcut_slots.lock().await.clone();
    write_device_settings(
        peripheral,
        &cbor::DeviceSettingsWrite {
            slot1: Some(shortcuts[0].clone()),
            slot2: Some(shortcuts[1].clone()),
            slot3: Some(shortcuts[2].clone()),
            ..Default::default()
        },
    )
    .await?;

    // Flush any clock-face/time-format/notification-filter edit made while
    // Ori was unreachable (pc-app.md's Disconnected-screen settings,
    // store::SavedState::pending_clock_face/pending_time_format/
    // pending_ancs_filter) — this is the first point after reconnecting
    // where a BLE write can land. Unlike shortcuts above, which are always
    // pushed regardless of local state, these three are normally
    // push-on-change-only (§6.3); a pending value here IS that change,
    // simply deferred from when the user made it to now that Ori can hear
    // it. Ordinary "nothing pending" reconnects skip this entirely — the
    // three fields keep whatever Ori itself already has, read back by the
    // frontend's own read_device_settings call once this sync completes.
    if let Some(mut saved) = crate::store::load(app).await {
        let has_pending = saved.pending_clock_face.is_some()
            || saved.pending_time_format.is_some()
            || saved.pending_ancs_filter.is_some();
        if has_pending {
            write_device_settings(
                peripheral,
                &cbor::DeviceSettingsWrite {
                    clock_face: saved.pending_clock_face,
                    time_format: saved.pending_time_format,
                    ancs_filter: saved.pending_ancs_filter,
                    ..Default::default()
                },
            )
            .await?;
            saved.pending_clock_face = None;
            saved.pending_time_format = None;
            saved.pending_ancs_filter = None;
            let _ = crate::store::save(app, &saved).await;
        }
    }

    let epoch_utc = now_epoch_secs();
    let tz = local_posix_tz();
    let time_sync_bytes = cbor::encode(&cbor::TimeSync { u: epoch_utc, z: &tz, x: process_clock_ms() });

    let profile_bytes = cbor::encode(&cbor::ProfileInfo {
        n: &profile.name,
        t: &profile.title,
        e: &profile.email,
        p: &profile.phone,
    });
    // No calendar source connected yet at this point in the flow — honestly
    // empty, not filled with placeholder data (meeting-list.md's own
    // fallback for an empty list is "No meetings today", which is the truth
    // here). The photo, though, is real if the user picked one in the
    // wizard's crop tool.
    //
    // `decode_cached_photo` self-heals rather than aborting the whole sync
    // (the old `decode_profile_photo(url)?`) on a bad decode here: unlike a
    // freshly-submitted photo — which `save_profile`/`save_timeoff` already
    // validate through this same decoder *before* it's ever persisted
    // (ble-protocol.md §10) — this is already-persisted `store::SavedState`
    // data (e.g. corrupted by a crash mid-`store::save`) that never changes
    // on its own. Aborting via `?` here used to make every future reconnect
    // hit the identical decode failure forever — a permanent, silent
    // reconnect-retry loop with no way to self-heal short of the user
    // happening to re-pick a new photo. Treating it as empty for this sync
    // (and clearing the persisted copy just below) lets the rest of
    // profile/meetings/Time Off still sync successfully.
    let (photo_bytes, photo_corrupted) = decode_cached_photo(decode_profile_photo, &profile.photo_data_url);
    // Still honestly empty — no calendar source exists yet to have real
    // data from (Phase D). Once it does, this needs the same "use cached/
    // fresh real data, not a hardcoded placeholder" treatment Time Off
    // just got below, or a reconnect would silently erase real meetings
    // the same way it used to erase Time Off.
    let meetings_bytes = cbor::encode(&cbor::MeetingList { d: local_midnight_epoch(), m: &[] });

    let (time_off_photo_bytes, time_off_photo_corrupted) =
        decode_cached_photo(decode_time_off_photo, &time_off.photo_data_url);
    let time_off_bytes = cbor::encode(&cbor::TimeOffEntry {
        s: time_off.start.max(0) as u64,
        e: time_off.end.max(0) as u64,
        d: &time_off.destination,
        m: &time_off_photo_bytes,
    });

    // Persist the correction right away — independent of whether the rest of
    // this particular sync goes on to succeed — so a corrupted cached photo
    // is a one-time event instead of a wedge every future reconnect hits
    // identically. A `None` load (nothing on disk yet, e.g. mid first-pair,
    // or the file was already cleared) just means there's nothing to correct.
    if photo_corrupted || time_off_photo_corrupted {
        if let Some(mut saved) = crate::store::load(app).await {
            if photo_corrupted {
                saved.profile.photo_data_url = None;
            }
            if time_off_photo_corrupted {
                saved.time_off.photo_data_url = None;
            }
            let _ = crate::store::save(app, &saved).await;
        }
    }

    let manifest = cbor::SyncManifestWrite {
        p: sha256(&profile_bytes),
        h: sha256(&photo_bytes),
        m: sha256(&meetings_bytes),
        t: sha256(&time_off_bytes),
    };
    peripheral
        .write(&chr_manifest, &cbor::encode(&manifest), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;
    emit_progress(app, 15.0, false, initial_pair);

    let needed = wait_for_manifest(&mut notifications, &chr_manifest)
        .await
        .unwrap_or_else(|| {
            ["profile", "photo", "meetings", "to"].iter().map(|s| s.to_string()).collect()
        });
    let is_needed = |key: &str| needed.iter().any(|n| n == key);

    let total = time_sync_bytes.len() as u64
        + if is_needed("profile") { profile_bytes.len() as u64 } else { 0 }
        + if is_needed("photo") { photo_bytes.len() as u64 } else { 0 }
        + if is_needed("meetings") { meetings_bytes.len() as u64 } else { 0 }
        + if is_needed("to") { time_off_bytes.len() as u64 } else { 0 };

    const SEQ: u32 = 1;
    peripheral
        .write(&chr_syncctl, &cbor::encode(&cbor::SyncControlBegin::new(SEQ, total)), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;
    emit_progress(app, 25.0, false, initial_pair);

    peripheral
        .write(&chr_time, &time_sync_bytes, WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;

    if is_needed("profile") {
        peripheral
            .write(&chr_profile, &profile_bytes, WriteType::WithResponse)
            .await
            .map_err(|e| e.to_string())?;
    }
    emit_progress(app, 45.0, false, initial_pair);

    if is_needed("photo") {
        write_chunked(peripheral, &chr_photo, &photo_bytes, &mut notifications, &chr_syncctl).await?;
    }
    emit_progress(app, 60.0, false, initial_pair);

    if is_needed("meetings") {
        write_chunked(peripheral, &chr_meetings, &meetings_bytes, &mut notifications, &chr_syncctl).await?;
    }
    emit_progress(app, 75.0, false, initial_pair);

    if is_needed("to") {
        write_chunked(peripheral, &chr_timeoff, &time_off_bytes, &mut notifications, &chr_syncctl).await?;
    }
    emit_progress(app, 90.0, false, initial_pair);

    peripheral
        .write(&chr_syncctl, &cbor::encode(&cbor::SyncControlEnd::new(SEQ)), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;

    wait_for_sync_complete(&mut notifications, &chr_status, SYNC_COMPLETE_TIMEOUT).await?;
    emit_progress(app, 100.0, true, initial_pair);

    // Cached for the Ori Info/Stats modal (`get_ori_info`) — a sync just
    // genuinely completed, which is the one moment both are cheaply known:
    // the address doesn't change, but re-caching it here costs nothing and
    // avoids a separate cache-on-connect call site to keep in sync.
    let address = peripheral.address().to_string();
    let address_changed = state.cached_address.lock().await.as_deref() != Some(address.as_str());
    *state.cached_address.lock().await = Some(address.clone());
    *state.last_synced.lock().await = Some(std::time::Instant::now());
    // Write-through to disk only on an actual change — this runs on EVERY
    // sync/reconnect, and the address never changes for a given bond, so
    // after the first successful sync each session this is just an
    // equality check, not a disk write (`BleState`'s own doc comment).
    if address_changed {
        if let Some(mut saved) = crate::store::load(app).await {
            saved.address = Some(address);
            let _ = crate::store::save(app, &saved).await;
        }
    }
    Ok(())
}

/// Live-reads Device Settings from Ori (char 000E) to recover the six
/// NVS-persisted fields — clock_face, time_format, ancs_filter, and the
/// three shortcut slot tokens (ble-protocol.md §6.4). Presence/weather are
/// excluded from Ori's own read response — Orion is their source of truth.
/// The frontend calls this on every `setConn('on')` transition
/// (`readSlotsFromDevice()` in app.js), matching "read on (re)connect" —
/// and again on a ~3 s poll while the Ori Info modal is open, for live
/// signal bars (pc-app.md).
///
/// `serial_number`/`manufacture_date`, when present, are write-through
/// cached (session + disk) the same way `run_sync` caches `address` — only
/// on an actual change, so the modal's poll doesn't turn into a disk write
/// every 3 s once the value is already known (`BleState`'s own doc comment).
pub async fn read_device_settings(app: &AppHandle, state: &BleState) -> Result<cbor::DeviceSettingsRead, String> {
    let peripheral = require_peripheral(state).await?;
    let chr = find_char(&peripheral, gatt::CHR_DEVICE_SETTINGS)?;
    let raw = peripheral.read(&chr).await.map_err(|e| e.to_string())?;
    let settings: cbor::DeviceSettingsRead = cbor::decode(&raw)?;

    let mut to_persist: Option<(Option<String>, Option<String>)> = None;
    if let Some(sn) = &settings.serial_number {
        let mut cached = state.cached_serial_number.lock().await;
        if cached.as_deref() != Some(sn.as_str()) {
            *cached = Some(sn.clone());
            to_persist.get_or_insert((None, None)).0 = Some(sn.clone());
        }
    }
    if let Some(mfg) = &settings.manufacture_date {
        let mut cached = state.cached_manufacture_date.lock().await;
        if cached.as_deref() != Some(mfg.as_str()) {
            *cached = Some(mfg.clone());
            to_persist.get_or_insert((None, None)).1 = Some(mfg.clone());
        }
    }
    if let Some((sn, mfg)) = to_persist {
        if let Some(mut saved) = crate::store::load(app).await {
            if sn.is_some() {
                saved.serial_number = sn;
            }
            if mfg.is_some() {
                saved.manufacture_date = mfg;
            }
            let _ = crate::store::save(app, &saved).await;
        }
    }

    Ok(settings)
}

async fn write_device_settings(peripheral: &Peripheral, settings: &cbor::DeviceSettingsWrite) -> Result<(), String> {
    let chr = find_char(peripheral, gatt::CHR_DEVICE_SETTINGS)?;
    peripheral
        .write(&chr, &cbor::encode(settings), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())
}

/// Writes Device Settings (char 000E) to Ori outside the BEGIN/END pipeline
/// — applied immediately (ble-protocol.md §6.4). Orion can write any subset
/// of fields in one call: presence and weather are ephemeral (pushed on
/// every (re)connect and on every change once a real source exists);
/// shortcuts/clock face/time format/ANCS filter are user-changed and
/// NVS-persisted on Ori. Used by the `save_device_settings` command — ready
/// for Phase D's Teams-presence and weather-API polling to call with real
/// values once those sources exist; until then, simply never being called
/// for `p`/`w`/`d`/`u` leaves Ori on its own honest fallback (Offline /
/// hidden weather), matching §6.4's "don't show what can't be verified"
/// policy.
pub async fn set_device_settings(state: &BleState, settings: cbor::DeviceSettingsWrite) -> Result<(), String> {
    let peripheral = require_peripheral(state).await?;
    write_device_settings(&peripheral, &settings).await?;

    // Session memory for the media bridge's `shortcut` dispatch (§12) — no
    // local settings store yet, so this only tracks what Orion itself
    // wrote this run, not what survives a restart.
    if settings.slot1.is_some() || settings.slot2.is_some() || settings.slot3.is_some() {
        let mut slots = state.shortcut_slots.lock().await;
        if let Some(s) = settings.slot1 {
            slots[0] = s;
        }
        if let Some(s) = settings.slot2 {
            slots[1] = s;
        }
        if let Some(s) = settings.slot3 {
            slots[2] = s;
        }
    }
    Ok(())
}

/// Distinguishes a mid-session BEGIN/END from the initial-pairing sync's
/// seq — Ori doesn't require global uniqueness across sessions, only that
/// a given session's BEGIN and END share one seq (ble-protocol.md §6.0).
const MID_SESSION_SEQ: u32 = 2;

/// Pushes a profile update through its own BEGIN/END session — same
/// mechanism `run_sync` uses for the initial pairing sync (§6.0), just
/// scoped to only Profile Info (+ Photo, if the user actually changed it)
/// rather than resending Meetings/Time Off too (§6.3 "Profile Info / Photo
/// | User edit in Orion | Hash-check, push if needed" — here we already
/// know it's needed, since the user just clicked Save, so there's no
/// manifest round-trip either).
pub async fn push_profile(state: &BleState, profile: &ProfileInput) -> Result<(), String> {
    let peripheral = require_peripheral(state).await?;

    let chr_status = find_char(&peripheral, gatt::CHR_DEVICE_STATUS)?;
    let chr_time = find_char(&peripheral, gatt::CHR_TIME_SYNC)?;
    let chr_profile = find_char(&peripheral, gatt::CHR_PROFILE_INFO)?;
    let chr_photo = find_char(&peripheral, gatt::CHR_PROFILE_PHOTO)?;
    let chr_syncctl = find_char(&peripheral, gatt::CHR_SYNC_CONTROL)?;

    // try_lock, not lock().await — this is only ever called as the
    // best-effort "push it right now if convenient" tail of commands.rs's
    // save_profile, which already persisted the edit to disk (the real
    // source of truth) before ever reaching here. If a bulk sync (initial
    // pair or reconnect delta) is already using the link, that session can
    // run for SYNC_COMPLETE_TIMEOUT (~30s) — waiting on it here used to make
    // clicking Save appear to hang for that long even though the edit was
    // already safely saved. Skipping cleanly when contended costs nothing:
    // the persisted edit reaches Ori on the next reconnect's hash-manifest
    // delta regardless (§6.2), exactly the same fallback already relied on
    // for a "not connected" failure here.
    let Ok(_sync_guard) = state.sync_lock.try_lock() else {
        return Ok(());
    };

    peripheral.subscribe(&chr_status).await.map_err(|e| e.to_string())?;
    let mut notifications: NotifyStream = peripheral.notifications().await.map_err(|e| e.to_string())?;

    let time_sync_bytes = cbor::encode(&cbor::TimeSync {
        u: now_epoch_secs(),
        z: &local_posix_tz(),
        x: process_clock_ms(),
    });
    let profile_bytes = cbor::encode(&cbor::ProfileInfo {
        n: &profile.name,
        t: &profile.title,
        e: &profile.email,
        p: &profile.phone,
    });
    // Only touch the photo characteristic if the user actually changed it
    // (picked a new one, or explicitly removed it) — an untouched item
    // inside a BEGIN/END session simply isn't staged, so Ori's existing
    // stored photo is left alone (§6.0).
    let photo_bytes = match &profile.photo_data_url {
        Some(url) => Some(decode_profile_photo(url)?),
        None if profile.photo_removed => Some(Vec::new()),
        None => None,
    };

    let total = time_sync_bytes.len() as u64
        + profile_bytes.len() as u64
        + photo_bytes.as_ref().map(|b| b.len() as u64).unwrap_or(0);

    peripheral
        .write(&chr_syncctl, &cbor::encode(&cbor::SyncControlBegin::new(MID_SESSION_SEQ, total)), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;
    peripheral
        .write(&chr_time, &time_sync_bytes, WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;
    peripheral
        .write(&chr_profile, &profile_bytes, WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;
    if let Some(photo) = &photo_bytes {
        write_chunked(&peripheral, &chr_photo, photo, &mut notifications, &chr_syncctl).await?;
    }
    peripheral
        .write(&chr_syncctl, &cbor::encode(&cbor::SyncControlEnd::new(MID_SESSION_SEQ)), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;

    wait_for_sync_complete(&mut notifications, &chr_status, SYNC_COMPLETE_TIMEOUT).await
}

/// Pushes a Time Off update the same way `push_profile` pushes a profile
/// update — its own BEGIN/END session scoped to just Time Off Entry (+
/// Photo if changed). `clear_time_off` reuses this with the zeroed
/// "inactive" entry described in `run_sync`'s comment on `time_off_bytes`.
pub async fn push_time_off(state: &BleState, input: &TimeOffInput) -> Result<(), String> {
    let peripheral = require_peripheral(state).await?;

    let chr_status = find_char(&peripheral, gatt::CHR_DEVICE_STATUS)?;
    let chr_time = find_char(&peripheral, gatt::CHR_TIME_SYNC)?;
    let chr_timeoff = find_char(&peripheral, gatt::CHR_TIME_OFF_ENTRY)?;
    let chr_syncctl = find_char(&peripheral, gatt::CHR_SYNC_CONTROL)?;

    // try_lock, not lock().await — see push_profile's identical comment.
    // Both its callers (save_timeoff/clear_timeoff via commands.rs) already
    // persisted to disk first, so skipping cleanly when a bulk sync is
    // already using the link costs nothing; the next reconnect's
    // hash-manifest delta picks it up regardless.
    let Ok(_sync_guard) = state.sync_lock.try_lock() else {
        return Ok(());
    };

    peripheral.subscribe(&chr_status).await.map_err(|e| e.to_string())?;
    let mut notifications: NotifyStream = peripheral.notifications().await.map_err(|e| e.to_string())?;

    let time_sync_bytes = cbor::encode(&cbor::TimeSync {
        u: now_epoch_secs(),
        z: &local_posix_tz(),
        x: process_clock_ms(),
    });
    let photo_bytes = match &input.photo_data_url {
        Some(url) => decode_time_off_photo(url)?,
        None if input.photo_removed => Vec::new(),
        None => Vec::new(),
    };
    let time_off_bytes = cbor::encode(&cbor::TimeOffEntry {
        s: input.start.max(0) as u64,
        e: input.end.max(0) as u64,
        d: &input.destination,
        m: &photo_bytes,
    });

    let total = time_sync_bytes.len() as u64 + time_off_bytes.len() as u64;

    peripheral
        .write(&chr_syncctl, &cbor::encode(&cbor::SyncControlBegin::new(MID_SESSION_SEQ, total)), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;
    peripheral
        .write(&chr_time, &time_sync_bytes, WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;
    write_chunked(&peripheral, &chr_timeoff, &time_off_bytes, &mut notifications, &chr_syncctl).await?;
    peripheral
        .write(&chr_syncctl, &cbor::encode(&cbor::SyncControlEnd::new(MID_SESSION_SEQ)), WriteType::WithResponse)
        .await
        .map_err(|e| e.to_string())?;

    wait_for_sync_complete(&mut notifications, &chr_status, SYNC_COMPLETE_TIMEOUT).await
}

pub async fn clear_time_off(state: &BleState) -> Result<(), String> {
    push_time_off(
        state,
        &TimeOffInput { start: 0, end: 0, destination: String::new(), photo_data_url: None, photo_removed: true },
    )
    .await
}

/// Drains any Sync Control notification already queued on `notifications`
/// over a short window, looking for a NACK (ble-protocol.md §5/§8: a chunk
/// gap or reassembly timeout on Ori's side). Doesn't block indefinitely —
/// absence of a NACK within the window just means the transfer landed clean.
async fn check_for_nack(notifications: &mut NotifyStream, chr_syncctl: &Characteristic) -> Option<String> {
    let deadline = tokio::time::sleep(Duration::from_millis(150));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            maybe_notif = notifications.next() => {
                let notif = maybe_notif?;
                if notif.uuid == chr_syncctl.uuid {
                    if let Ok(parsed) = cbor::decode::<cbor::SyncControlNotify>(&notif.value) {
                        if parsed.o == "NACK" {
                            return Some(parsed.r.unwrap_or_else(|| "unknown".into()));
                        }
                    }
                }
            }
            _ = &mut deadline => return None,
        }
    }
}

/// Drains any Sync Control notifications already sitting in the channel,
/// without waiting for anything new — called right before a new item's
/// chunked transfer starts, so a NACK that arrived *late* for the
/// *previous* item (after that item's own `check_for_nack` window already
/// gave up and moved on) can't silently get misattributed to this one
/// instead. The wire protocol (ble-protocol.md §5/§8) carries no item
/// identity on a NACK — only a `seq` scoped to whichever item is currently
/// mid-transfer, which a fragment index near either item's start/end can't
/// reliably disambiguate either — so a notification Orion can't otherwise
/// place is safest treated as stale and discarded here rather than
/// attributed to the wrong item (a needless full retransmit at best, a
/// masked real failure on the *actual* item at worst).
async fn drain_stale_notifications(notifications: &mut NotifyStream) {
    while tokio::time::timeout(Duration::ZERO, notifications.next()).await.is_ok() {
        // Drained one — no way to tell which now-past item this was
        // really for, so it's simply discarded rather than guessed at.
    }
}

/// Streams `payload` as chunk frames (§5), then checks for a NACK
/// (`chunk_missing`/`chunk_timeout`/`cbor_decode`) before returning. On a
/// NACK, restarts the whole item from seq=0 once (§5: "sender restarts from
/// seq=0"); a second NACK is treated as a real failure rather than retried
/// forever, so the caller's sync fails loudly instead of the outer
/// `wait_for_sync_complete` timing out with no idea why.
async fn write_chunked(
    peripheral: &Peripheral,
    chr: &Characteristic,
    payload: &[u8],
    notifications: &mut NotifyStream,
    chr_syncctl: &Characteristic,
) -> Result<(), String> {
    drain_stale_notifications(notifications).await;
    let frag_size = chunk::frag_size_for_mtu(peripheral.mtu());
    let mut attempt = 0;
    loop {
        let frames = chunk::make_frames(payload, frag_size);
        let total = frames.len();
        for (i, frame) in frames.iter().enumerate() {
            let write_type = if chunk::is_checkpoint(i, total) {
                WriteType::WithResponse
            } else {
                WriteType::WithoutResponse
            };
            peripheral.write(chr, frame, write_type).await.map_err(|e| e.to_string())?;
        }
        match check_for_nack(notifications, chr_syncctl).await {
            None => return Ok(()),
            Some(reason) => {
                attempt += 1;
                if attempt >= 2 {
                    return Err(format!("Ori rejected the transfer twice ({reason}) — giving up"));
                }
                // Loop back and resend this item from seq=0.
            }
        }
    }
}

/// Media Album Art (char 000D) advertises Write-No-Response only — no
/// Write-with-response property exists on it (ble-protocol.md §3), so
/// `write_chunked`'s ack-based checkpoint isn't available. Every frame goes
/// Write-No-Response; the only overrun guard is a short pause every
/// `CHUNK_WINDOW` frames, mirroring `tools/mock_orion_ble.py`'s
/// `write_chunked_nr()`. Acceptable here since album art is small (≤ 64 KB)
/// and non-critical — a dropped frame just means a stale/blank art tile.
async fn write_chunked_no_response(peripheral: &Peripheral, chr: &Characteristic, payload: &[u8]) -> Result<(), String> {
    let frames = chunk::make_frames(payload, chunk::frag_size_for_mtu(peripheral.mtu()));
    for (i, frame) in frames.iter().enumerate() {
        peripheral.write(chr, frame, WriteType::WithoutResponse).await.map_err(|e| e.to_string())?;
        if (i + 1) % chunk::CHUNK_WINDOW == 0 {
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    }
    Ok(())
}

async fn wait_for_manifest(notifications: &mut NotifyStream, chr_manifest: &Characteristic) -> Option<Vec<String>> {
    let deadline = tokio::time::sleep(MANIFEST_REPLY_TIMEOUT);
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            maybe_notif = notifications.next() => {
                let notif = maybe_notif?;
                if notif.uuid == chr_manifest.uuid {
                    if let Ok(parsed) = cbor::decode::<cbor::SyncManifestNotify>(&notif.value) {
                        return Some(parsed.n);
                    }
                }
            }
            _ = &mut deadline => return None,
        }
    }
}

async fn wait_for_sync_complete(
    notifications: &mut NotifyStream,
    chr_status: &Characteristic,
    timeout: Duration,
) -> Result<(), String> {
    let deadline = tokio::time::sleep(timeout);
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            maybe_notif = notifications.next() => {
                let notif = maybe_notif.ok_or("BLE link dropped while waiting for sync to complete")?;
                if notif.uuid == chr_status.uuid {
                    if let Some(&byte) = notif.value.first() {
                        if gatt::DeviceStatus::from(byte).is_sync_complete() {
                            return Ok(());
                        }
                    }
                }
            }
            _ = &mut deadline => return Err("timed out waiting for Ori to finish syncing".into()),
        }
    }
}

/// `initial_pair` picks which event this fires as — see `run_sync`'s doc
/// comment on the parameter for why a reconnect must never emit
/// `sync-progress` (the setup wizard's own event, which finishes the wizard
/// and stomps the profile card on `done`). `resync-progress` currently has
/// no frontend listener, which is correct: `setConn`'s existing "rec"/
/// Syncing state is Orion's whole reconnect UI (pc-app.md's panel layout has
/// no reconnect progress ring of its own), so a routine background sync
/// completing should be invisible beyond that.
fn emit_progress(app: &AppHandle, pct: f32, done: bool, initial_pair: bool) {
    let event = if initial_pair { "sync-progress" } else { "resync-progress" };
    let _ = app.emit(event, SyncProgressEvent { pct, label: None, done });
}

fn sha256(data: &[u8]) -> Vec<u8> {
    Sha256::digest(data).to_vec()
}

/// Ori's Profile Photo cap: JPEG, 228×228, hard cap 200 KB (ble-protocol.md
/// §10). The setup wizard's crop tool already produces exactly that (fixed
/// 228×228 canvas, JPEG quality 0.92).
const PROFILE_PHOTO_MAX_BYTES: usize = 200 * 1024;

/// Ori's Time Off destination photo cap: JPEG, 528×396, hard cap 512 KB
/// (ble-protocol.md §10). Same crop-tool treatment, different target size.
const TIME_OFF_PHOTO_MAX_BYTES: usize = 512 * 1024;

/// Decodes a `data:image/jpeg;base64,...` URL, enforces `max_bytes`, and
/// verifies the bytes actually decode as an image — rather than trusting the
/// frontend's crop tool blindly. This is the one validation gate malformed or
/// oversized photo data must pass before it's allowed anywhere near
/// `store::save` — `run_sync` re-decodes whatever's cached here on *every*
/// reconnect, so bad bytes that slipped past this check would silently wedge
/// every future sync, not just the one that introduced them (see
/// `commands::save_profile`/`save_timeoff`, which call the `pub` wrappers
/// below to validate before persisting anything to disk).
fn decode_photo_data_url(data_url: &str, max_bytes: usize, what: &str) -> Result<Vec<u8>, String> {
    use base64::prelude::{Engine as _, BASE64_STANDARD};

    let (_, encoded) = data_url.split_once(',').ok_or_else(|| format!("malformed {what} data URL"))?;
    let bytes = BASE64_STANDARD.decode(encoded).map_err(|e| format!("couldn't decode {what}: {e}"))?;
    if bytes.len() > max_bytes {
        return Err(format!(
            "{what} is {} KB, over Ori's {} KB cap (ble-protocol.md §10)",
            bytes.len() / 1024,
            max_bytes / 1024
        ));
    }
    // Actually decode as an image (the `image` crate, already a dependency —
    // see `build_album_art_jpeg` below) rather than just trusting the base64
    // decoded into *something* — a corrupt or non-image payload that passed
    // the size check would otherwise reach Ori's decode step at sync time
    // instead of failing here, up front, where the caller can reject it.
    image::load_from_memory(&bytes).map_err(|e| format!("{what} isn't a valid image: {e}"))?;
    Ok(bytes)
}

pub fn decode_profile_photo(data_url: &str) -> Result<Vec<u8>, String> {
    decode_photo_data_url(data_url, PROFILE_PHOTO_MAX_BYTES, "profile photo")
}

pub fn decode_time_off_photo(data_url: &str) -> Result<Vec<u8>, String> {
    decode_photo_data_url(data_url, TIME_OFF_PHOTO_MAX_BYTES, "Time Off photo")
}

/// Decodes a *cached* (already-persisted) photo for `run_sync`'s use,
/// self-healing instead of failing the whole sync if the cached bytes turn
/// out to be corrupt — e.g. a partial disk write from a crash mid-
/// `store::save`, not a photo the user just submitted (that path is already
/// gated through `decode_profile_photo`/`decode_time_off_photo` before it's
/// ever persisted — see `commands::save_profile`/`save_timeoff`). Returns
/// `(bytes, was_corrupted)`: on success, the decoded bytes and `false`; on a
/// decode failure, an empty `Vec` (so the caller can still proceed with the
/// rest of the sync) and `true` so the caller can clear the persisted field —
/// otherwise the identical decode failure would repeat on every future
/// reconnect forever.
fn decode_cached_photo(decode: impl Fn(&str) -> Result<Vec<u8>, String>, url: &Option<String>) -> (Vec<u8>, bool) {
    match url {
        Some(u) => match decode(u) {
            Ok(bytes) => (bytes, false),
            Err(_) => (Vec::new(), true),
        },
        None => (Vec::new(), false),
    }
}

/// Media Album Art target: 484×216, hard cap 64 KB (ble-protocol.md §10/§12).
const ALBUM_ART_TARGET_W: u32 = 484;
const ALBUM_ART_TARGET_H: u32 = 216;
const ALBUM_ART_MAX_BYTES: usize = 64 * 1024;

/// Center-crops `raw` (whatever format the OS thumbnail came back as — JPEG/
/// PNG/BMP in practice) to the 484:216 aspect ratio, resizes to exactly that,
/// and re-encodes as JPEG under the 64 KB cap — mirrors
/// `tools/mock_orion_ble.py`'s `_crop_resize_to_aspect()` + `_jpeg_max_quality()`.
/// `Ok(Vec::new())` (not an error) for empty input or a source Ori can't decode —
/// both just mean "no art to show this track," not a failure worth surfacing.
fn build_album_art_jpeg(raw: &[u8]) -> Vec<u8> {
    if raw.is_empty() {
        return Vec::new();
    }
    let Ok(img) = image::load_from_memory(raw) else { return Vec::new() };
    let (w, h) = (img.width(), img.height());
    let cropped = if w * ALBUM_ART_TARGET_H > h * ALBUM_ART_TARGET_W {
        // Source too wide relative to target — crop the sides.
        let nw = h * ALBUM_ART_TARGET_W / ALBUM_ART_TARGET_H;
        img.crop_imm((w - nw) / 2, 0, nw, h)
    } else {
        // Source too tall relative to target — crop top/bottom.
        let nh = w * ALBUM_ART_TARGET_H / ALBUM_ART_TARGET_W;
        img.crop_imm(0, (h - nh) / 2, w, nh)
    };
    let resized = cropped.resize_exact(ALBUM_ART_TARGET_W, ALBUM_ART_TARGET_H, image::imageops::FilterType::Lanczos3);
    let rgb = resized.to_rgb8();

    use image::ImageEncoder;
    // Binary search the highest quality (10..=95) that fits under the cap —
    // JPEG size grows monotonically with quality for a fixed image, so this
    // finds the same answer the old linear 95-down-to-10 scan did (the
    // highest quality that still fits) in ~7 encodes instead of up to 86.
    let mut lo: i32 = 10;
    let mut hi: i32 = 95;
    let mut best: Option<Vec<u8>> = None;
    while lo <= hi {
        let mid = (lo + hi) / 2;
        let mut buf = Vec::new();
        let encoder = image::codecs::jpeg::JpegEncoder::new_with_quality(&mut buf, mid as u8);
        let fits = encoder.write_image(rgb.as_raw(), rgb.width(), rgb.height(), image::ExtendedColorType::Rgb8).is_ok()
            && buf.len() <= ALBUM_ART_MAX_BYTES;
        if fits {
            best = Some(buf);
            lo = mid + 1; // try for higher quality
        } else {
            hi = mid - 1; // too big (or failed) — go lower
        }
    }
    best.unwrap_or_default()
}

fn now_epoch_secs() -> u64 {
    // `.unwrap_or_default()`, not `.expect(...)` — this runs on every sync/
    // reconnect path, with nothing above it in the call chain prepared to
    // catch a panic. A misconfigured system clock reading before 1970 used
    // to unwind straight through `supervise_connection_loop` and permanently
    // end the whole supervisor task (nothing re-spawns one except a fresh
    // pairing or an app restart) — a bad clock is bad enough without also
    // silently killing background reconnection for the rest of the session.
    // Falling back to epoch 0 is honestly wrong either way; it just fails
    // safe instead of fatal.
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn process_clock_ms() -> u64 {
    static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    START.get_or_init(std::time::Instant::now).elapsed().as_millis() as u64
}

/// Minutes to ADD to local time to reach UTC (Windows' own `Bias`
/// convention) — this is directly the POSIX TZ sign convention Ori's
/// newlib `tzset()` expects (ble-protocol.md §4 TimeSync.z).
#[cfg(target_os = "windows")]
fn utc_offset_minutes() -> i32 {
    use windows::Win32::System::Time::{GetTimeZoneInformation, TIME_ZONE_INFORMATION};
    const TIME_ZONE_ID_DAYLIGHT: u32 = 2;
    unsafe {
        let mut info = TIME_ZONE_INFORMATION::default();
        let result = GetTimeZoneInformation(&mut info);
        let extra = if result == TIME_ZONE_ID_DAYLIGHT { info.DaylightBias } else { info.StandardBias };
        info.Bias + extra
    }
}

#[cfg(not(target_os = "windows"))]
fn utc_offset_minutes() -> i32 {
    0
}

/// POSIX TZ string for this machine's current local UTC offset — no DST
/// transition rule, just the offset in effect right now (matches
/// tools/mock_orion_ble.py's `_local_posix_tz()`; ble-protocol.md §4 calls
/// out "LOC-2"-style fixed-offset strings as an explicitly valid form).
fn local_posix_tz() -> String {
    let posix_min = utc_offset_minutes();
    let sign = if posix_min < 0 { "-" } else { "" };
    let hh = posix_min.abs() / 60;
    let mm = posix_min.abs() % 60;
    if mm != 0 {
        format!("LOC{sign}{hh}:{mm:02}")
    } else {
        format!("LOC{sign}{hh}")
    }
}

/// Epoch (UTC) of this machine's local midnight, today — see
/// tools/mock_orion_ble.py's `_local_midnight_epoch()` for why this must be
/// local-wall-clock midnight, not UTC midnight.
fn local_midnight_epoch() -> u64 {
    let offset_secs = i64::from(utc_offset_minutes()) * 60; // UTC = local + offset_secs
    let now = now_epoch_secs() as i64;
    let local_now = now - offset_secs;
    let local_midnight = local_now - local_now.rem_euclid(86400);
    (local_midnight + offset_secs) as u64
}
