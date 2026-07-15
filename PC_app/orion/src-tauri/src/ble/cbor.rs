// CBOR payload schemas — ble-protocol.md §4. Keys are single characters, per
// the spec ("tiny, frequent payloads ... no human ever reading raw CBOR off
// the wire"). Mirrors tools/mock_orion_ble.py's payload builders field-for-
// field so the two implementations stay wire-compatible.
//
// `#![allow(dead_code)]`: nearly every schema here is live (KeyboardCommand,
// HostVolumeState, DeviceSettingsRead, and SyncControlNotify's NACK path are
// all consumed — media bridge, Device Settings read-back, and chunk-transfer
// NACK detection respectively). `Meeting` (the single-item schema, as
// opposed to `MeetingList`) is the one genuine exception — meetings are
// always sent as an empty list until Phase D's calendar integration lands,
// so no `Meeting` ever actually gets constructed yet.
#![allow(dead_code)]

use serde::{Deserialize, Serialize};

pub fn encode<T: Serialize>(value: &T) -> Vec<u8> {
    let mut buf = Vec::new();
    ciborium::ser::into_writer(value, &mut buf).expect("CBOR encoding is infallible for these schemas");
    buf
}

pub fn decode<T: for<'de> Deserialize<'de>>(bytes: &[u8]) -> Result<T, String> {
    ciborium::de::from_reader(bytes).map_err(|e| e.to_string())
}

#[derive(Serialize)]
pub struct TimeSync<'a> {
    pub u: u64,
    pub z: &'a str,
    pub x: u64,
}

#[derive(Serialize)]
pub struct ProfileInfo<'a> {
    pub n: &'a str,
    pub t: &'a str,
    pub e: &'a str,
    pub p: &'a str,
}

#[derive(Serialize)]
pub struct Meeting<'a> {
    pub i: &'a str,
    pub s: u64,
    pub e: u64,
    pub t: &'a str,
    pub l: &'a str,
    pub o: &'a str,
}

#[derive(Serialize)]
pub struct MeetingList<'a> {
    pub d: u64,
    pub m: &'a [Meeting<'a>],
}

#[derive(Serialize)]
pub struct TimeOffEntry<'a> {
    pub s: u64,
    pub e: u64,
    pub d: &'a str,
    #[serde(with = "serde_bytes")]
    pub m: &'a [u8],
}

#[derive(Serialize)]
pub struct SyncControlBegin {
    pub o: &'static str,
    pub s: u32,
    pub t: u64,
}

impl SyncControlBegin {
    pub fn new(seq: u32, total: u64) -> Self {
        Self { o: "BEGIN", s: seq, t: total }
    }
}

#[derive(Serialize)]
pub struct SyncControlEnd {
    pub o: &'static str,
    pub s: u32,
}

impl SyncControlEnd {
    pub fn new(seq: u32) -> Self {
        Self { o: "END", s: seq }
    }
}

#[derive(Deserialize, Debug)]
pub struct SyncControlNotify {
    pub o: String,
    #[serde(default)]
    pub s: Option<u32>,
    #[serde(default)]
    pub r: Option<String>,
}

/// Orion → Ori — every section's SHA-256 (§6.2). No Device Settings entry —
/// shortcuts/presence/weather are written outside the BEGIN/END pipeline and
/// have no hash.
#[derive(Serialize)]
pub struct SyncManifestWrite {
    #[serde(with = "serde_bytes")]
    pub p: Vec<u8>,
    #[serde(with = "serde_bytes")]
    pub h: Vec<u8>,
    #[serde(with = "serde_bytes")]
    pub m: Vec<u8>,
    #[serde(with = "serde_bytes")]
    pub t: Vec<u8>,
}

/// Ori → Orion — subset of {"profile","photo","meetings","to"} that differs
/// from what Orion advertised.
#[derive(Deserialize, Debug, Default)]
pub struct SyncManifestNotify {
    #[serde(default)]
    pub n: Vec<String>,
}

/// Orion → Ori, write (response). All fields optional — absent keys leave
/// Ori's current state unchanged (§4/§6.4).
#[derive(Serialize, Default)]
pub struct DeviceSettingsWrite {
    #[serde(rename = "p", skip_serializing_if = "Option::is_none")]
    pub presence: Option<u8>,
    #[serde(rename = "1", skip_serializing_if = "Option::is_none")]
    pub slot1: Option<String>,
    #[serde(rename = "2", skip_serializing_if = "Option::is_none")]
    pub slot2: Option<String>,
    #[serde(rename = "3", skip_serializing_if = "Option::is_none")]
    pub slot3: Option<String>,
    #[serde(rename = "c", skip_serializing_if = "Option::is_none")]
    pub clock_face: Option<u8>,
    #[serde(rename = "h", skip_serializing_if = "Option::is_none")]
    pub time_format: Option<u8>,
    #[serde(rename = "f", skip_serializing_if = "Option::is_none")]
    pub ancs_filter: Option<u8>,
    #[serde(rename = "w", skip_serializing_if = "Option::is_none")]
    pub weather_condition: Option<u8>,
    #[serde(rename = "d", skip_serializing_if = "Option::is_none")]
    pub temperature: Option<i32>,
    #[serde(rename = "u", skip_serializing_if = "Option::is_none")]
    pub temperature_unit: Option<u8>,
}

/// Ori → Orion, read result — only the NVS-persisted fields come back
/// (presence/weather are ephemeral and excluded — Orion is their source of
/// truth, §6.4). Also re-serialized as-is back to the frontend (Tauri IPC
/// response for the `read_device_settings` command) — `skip_serializing_if`
/// keeps an absent field genuinely `undefined` on the JS side rather than
/// `null`, matching app.js's `s.c!==undefined` checks.
///
/// `serial_number`/`manufacture_date`/`signal_bars` back the Ori Info modal
/// (pc-app.md) — piggybacked on this characteristic rather than a new one
/// (ble-protocol.md §4/§6.4). The first two come from Ori's separate
/// write-once "factory" NVS partition; `signal_bars` is sampled live by Ori
/// on every read (Windows can't read RSSI of an already-connected
/// peripheral, so Ori — which can — reports its own bucketed reading back).
/// None of the three are ever sent on a write.
#[derive(Serialize, Deserialize, Debug, Default, Clone)]
pub struct DeviceSettingsRead {
    #[serde(rename = "c", skip_serializing_if = "Option::is_none")]
    pub clock_face: Option<u8>,
    #[serde(rename = "h", skip_serializing_if = "Option::is_none")]
    pub time_format: Option<u8>,
    #[serde(rename = "f", skip_serializing_if = "Option::is_none")]
    pub ancs_filter: Option<u8>,
    #[serde(rename = "1", skip_serializing_if = "Option::is_none")]
    pub slot1: Option<String>,
    #[serde(rename = "2", skip_serializing_if = "Option::is_none")]
    pub slot2: Option<String>,
    #[serde(rename = "3", skip_serializing_if = "Option::is_none")]
    pub slot3: Option<String>,
    #[serde(rename = "s", skip_serializing_if = "Option::is_none")]
    pub serial_number: Option<String>,
    #[serde(rename = "b", skip_serializing_if = "Option::is_none")]
    pub manufacture_date: Option<String>,
    #[serde(rename = "r", skip_serializing_if = "Option::is_none")]
    pub signal_bars: Option<u8>,
}

/// Ori → Orion, notify (char 000A) — play/pause/next/prev/vol_set/shortcut.
#[derive(Deserialize, Debug)]
pub struct KeyboardCommand {
    pub o: String,
    #[serde(default)]
    pub a: u32,
}

#[derive(Serialize, Deserialize, Debug)]
pub struct HostVolumeState {
    pub l: u8,
    pub m: bool,
}

fn is_false(b: &bool) -> bool {
    !*b
}

/// Orion → Ori, write (+ Ori can notify on it) — §4/§12. `playing` should
/// always be sent so Ori's icon stays in sync with the OS; `position_s`/
/// `duration_s` only on a track change or seek (both together or neither).
#[derive(Serialize, Default)]
pub struct MediaMetadata<'a> {
    pub t: &'a str,
    pub a: &'a str,
    #[serde(skip_serializing_if = "is_false")]
    pub c: bool,
    pub p: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub o: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub d: Option<u32>,
}

/// Ori → Orion, notify + readable (char 000F) — iPhone ANCS bond/connection
/// state plus live notification stats and signal. Ori notifies on every
/// change and keeps the characteristic value current, so a read on
/// (re)connect recovers the state without waiting for a notify
/// (ble-protocol.md §3/§11). `m`/`u`/`t`/`s`/`l` are always 0 while `c` is
/// false (`d` is "" instead, matching `n`).
#[derive(Serialize, Deserialize, Debug, Clone, Default)]
pub struct PhoneBondStatus {
    pub b: bool,
    pub c: bool,
    #[serde(default)]
    pub n: String,
    /// device_type — iPhone's marketing model name (e.g. "iPhone 17 Pro
    /// Max"), already resolved from Apple's raw hardware identifier by Ori
    /// (ble-protocol.md's PhoneBondStatus schema) — display as-is, no
    /// resolution needed on this side.
    #[serde(default)]
    pub d: String,
    #[serde(default)]
    pub m: u8,
    #[serde(default)]
    pub u: u8,
    #[serde(default)]
    pub t: u8,
    #[serde(default)]
    pub s: u8,
    /// battery_level — 0-100 (%).
    #[serde(default)]
    pub l: u8,
}

/// Ori → Orion, notify (char 0010, notify-only — no Read property) —
/// individual ANCS notification content, ble-protocol.md §13. Already
/// filter-gated by Ori's `ancs_filter` (Device Settings `"f"`) before it's
/// ever sent — Orion relays whatever arrives without a filter check of its
/// own. `"add"` carries every optional field (a genuinely-new notification,
/// or Orion's stored copy replaced in place on an ANCS Modified event, both
/// keyed by `u`); `"remove"` carries only `u`; `"clear"` carries neither
/// (wipe the whole local mirror — sent when the user changes `ancs_filter`,
/// followed by `"add"` for everything that now passes).
#[derive(Serialize, Deserialize, Debug, Clone, Default)]
pub struct AncsNotification {
    pub o: String,
    #[serde(default)]
    pub u: u32,
    #[serde(default)]
    pub k: String,
    #[serde(default)]
    pub c: u8,
    #[serde(default)]
    pub a: String,
    #[serde(default)]
    pub t: String,
    #[serde(default)]
    pub b: String,
    #[serde(default)]
    pub e: u32,
    #[serde(default)]
    pub p: String,
    #[serde(default)]
    pub n: String,
    #[serde(default)]
    pub g: bool,
    #[serde(default)]
    pub s: bool,
}

/// Ori → Orion, notify (char 0011, notify-only) — live call state
/// (ble-protocol.md §13). `st`: 0=none/ended 1=ringing 2=active. `e`
/// (elapsed_s) is only meaningful when `st == 2` — lets Orion's in-call
/// timer resume from the right value after a reconnect mid-call instead of
/// restarting at 00:00. `a`/`t`/`p`/`n`/`g` are the caller's identity +
/// action button labels (populated for st==1/2, empty/false for st==0) —
/// never fabricated locally, whatever Ori/ANCS actually labelled them.
#[derive(Serialize, Deserialize, Debug, Clone, Default)]
pub struct AncsCallState {
    pub st: u8,
    #[serde(default)]
    pub u: u32,
    #[serde(default)]
    pub e: u32,
    #[serde(default)]
    pub a: String,
    #[serde(default)]
    pub t: String,
    #[serde(default)]
    pub p: String,
    #[serde(default)]
    pub n: String,
    #[serde(default)]
    pub g: bool,
    /// Calling app's icon token (same vocabulary as `AncsNotification.k`) —
    /// lets the frontend render the real app icon (Viber/Phone/…) for the
    /// call instead of a generic glyph. Empty for `st == 0`. ble-protocol.md §13.
    #[serde(default)]
    pub k: String,
}

/// Orion → Ori, write (response) (char 0012) — Answer/Decline/End
/// call/Dismiss/Read-all (one write per uid for a stacked "Read all"),
/// ble-protocol.md §13 "Actions". `a`: 0=Positive 1=Negative.
#[derive(Serialize, Debug, Clone)]
pub struct AncsNotificationAction {
    pub u: u32,
    pub a: u8,
}
