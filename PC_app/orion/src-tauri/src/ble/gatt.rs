// GATT UUIDs — Ori Sync Service (ble-protocol.md §3) + the standard Device
// Information Service (§3.1). Mirrors tools/mock_orion_ble.py's UUID table
// byte-for-byte so the two implementations stay wire-compatible.
//
// `#![allow(dead_code)]`: this documents the full protocol table (every
// characteristic + advertising constant), not just what's referenced by
// name elsewhere — `SVC_ORI_SYNC` and `ADV_FLAG_RUNTIME` are genuinely
// unused today (the service UUID isn't scanned/filtered on directly, and
// only the `SETUP` flag is checked, not `RUNTIME`) but are kept for
// completeness against the spec.
#![allow(dead_code)]

pub const SVC_ORI_SYNC: &str = "6f726900-0000-4f72-9f00-000000000000";

pub const CHR_DEVICE_STATUS: &str = "6f726900-0001-4f72-9f00-000000000000";
pub const CHR_TIME_SYNC: &str = "6f726900-0002-4f72-9f00-000000000000";
pub const CHR_PROFILE_INFO: &str = "6f726900-0003-4f72-9f00-000000000000";
pub const CHR_PROFILE_PHOTO: &str = "6f726900-0004-4f72-9f00-000000000000";
pub const CHR_MEETING_LIST: &str = "6f726900-0005-4f72-9f00-000000000000";
pub const CHR_TIME_OFF_ENTRY: &str = "6f726900-0006-4f72-9f00-000000000000";
pub const CHR_SYNC_CONTROL: &str = "6f726900-0007-4f72-9f00-000000000000";
pub const CHR_DEVICE_COMMAND: &str = "6f726900-0008-4f72-9f00-000000000000";
pub const CHR_SYNC_MANIFEST: &str = "6f726900-0009-4f72-9f00-000000000000";
pub const CHR_KEYBOARD_COMMAND: &str = "6f726900-000a-4f72-9f00-000000000000";
pub const CHR_HOST_VOLUME_STATE: &str = "6f726900-000b-4f72-9f00-000000000000";
pub const CHR_MEDIA_METADATA: &str = "6f726900-000c-4f72-9f00-000000000000";
pub const CHR_MEDIA_ALBUM_ART: &str = "6f726900-000d-4f72-9f00-000000000000";
pub const CHR_DEVICE_SETTINGS: &str = "6f726900-000e-4f72-9f00-000000000000";
pub const CHR_PHONE_BOND_STATUS: &str = "6f726900-000f-4f72-9f00-000000000000";
/// ANCS relay to Orion (ble-protocol.md §13) — notify-only (no Read
/// property, unlike Phone Bond Status): Ori's per-notification content.
pub const CHR_ANCS_NOTIFICATION: &str = "6f726900-0010-4f72-9f00-000000000000";
/// ANCS relay to Orion (§13) — notify-only: live call state.
pub const CHR_ANCS_CALL_STATE: &str = "6f726900-0011-4f72-9f00-000000000000";
/// ANCS relay to Orion (§13) — Orion writes Answer/Decline/End-call/Dismiss.
pub const CHR_ANCS_NOTIFICATION_ACTION: &str = "6f726900-0012-4f72-9f00-000000000000";

/// Device Information Service (BLE SIG standard, 0x180A) — Firmware Revision
/// String (0x2A26). Separate service from Ori Sync Service — see §3.1.
pub const CHR_FW_REVISION: &str = "00002a26-0000-1000-8000-00805f9b34fb";

/// Advertising manufacturer-data company ID (placeholder, §2) and mode flag
/// values. `manufacturer_data` maps are keyed by company ID with the 2-byte
/// ID already stripped from the value, so `data[0]` is the flag byte.
pub const MFG_COMPANY_ID: u16 = 0xFFFF;
pub const ADV_FLAG_SETUP: u8 = 0x01;
pub const ADV_FLAG_RUNTIME: u8 = 0x02;

/// Device Command (char 0008) magic values — not CBOR (§3 table, 4-byte
/// payload, magic-routed).
pub const FACTORY_RESET_MAGIC: [u8; 4] = [0xFA, 0xC7, 0x5E, 0x5E];
pub const UNPAIR_PHONE_MAGIC: [u8; 4] = [0x55, 0x4E, 0x50, 0x52];

/// Device Status (char 0001) — single-byte enum, not CBOR (§3 table).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceStatus {
    SetupWaitingPairing,
    SetupBondedAwaitingSync,
    SetupSyncing,
    SetupSyncComplete,
    RuntimeReady,
    RuntimeReconnecting,
    RuntimeSyncing,
    ErrorGeneric,
    Unknown(u8),
}

impl From<u8> for DeviceStatus {
    fn from(byte: u8) -> Self {
        match byte {
            0x00 => Self::SetupWaitingPairing,
            0x01 => Self::SetupBondedAwaitingSync,
            0x02 => Self::SetupSyncing,
            0x03 => Self::SetupSyncComplete,
            0x10 => Self::RuntimeReady,
            0x11 => Self::RuntimeReconnecting,
            0x12 => Self::RuntimeSyncing,
            0xF0 => Self::ErrorGeneric,
            other => Self::Unknown(other),
        }
    }
}

impl DeviceStatus {
    /// True once a sync session (first-pair or reconnect) has committed.
    pub fn is_sync_complete(self) -> bool {
        matches!(self, Self::SetupSyncComplete | Self::RuntimeReady)
    }
}
