# Ori — BLE GATT Protocol Specification

**Contract revision:** 1.0 — pre-release, no version-history tracking. No wire-transmitted protocol version negotiation; see §9.
**Status:** Authoritative — `esp32-connectivity` (Ori firmware) and `orion-sync` (Orion app) must conform.

Defines the single BLE GATT contract between Ori and the Orion PC app.

**Out of scope for BLE:** ANCS phone link (`connectivity.md`); firmware updates run over USB CDC (`ota.md`).

---

## 1. Roles

| Side | Role |
|---|---|
| **Ori (Arduino on ESP32-S3)** | GATT server + Advertiser. Hosts every characteristic below. |
| **Orion** — one Tauri v2 (Rust) codebase, both Windows and macOS (`memory.md`) | GATT client via `btleplug`. The only side that can issue Read/Write requests. |

LE Secure Connections with Passkey Entry (6-digit numeric, MITM-protected) is mandatory. After first pairing the device is bonded; reconnects are silent. On Windows, Orion owns the passkey-entry UI (custom digit-box modal driving WinRT's `DeviceInformationCustomPairing`) rather than the OS's default pairing flyout — see §6.1 and `memory.md`. macOS has no equivalent app-level pairing hook and is deferred until that build starts.

---

## 2. Advertising and bond policy

Ori accepts **at most two bonded peers**: one PC (Orion) and one iPhone (ANCS). New bonds are accepted only in the state-gated pairing windows below; outside them an unknown device can connect but can neither bond nor read/write any (encrypted) data characteristic. When both slots are full and both peers are connected, Ori stops advertising; if a bonded peer drops, Ori advertises **public undirected** so it can reconnect on its own.

### Bond slot enforcement

1. **State-gated pairing.**

   | Firmware state | Bond slot open |
   |---|---|
   | Setup Step 2 | Orion slot only |
   | Setup Step 4 / Runtime re-pair-iPhone | iPhone slot only |
   | All other states | Neither — rejected |

2. **Typed NVS slots.**

   ```
   NVS namespace "bonds":
     "orion_addr"  → 6-byte BLE address  (all-zero = slot empty)
     "iphone_addr" → 6-byte BLE address  (all-zero = slot empty)
   ```

   Step 2 bond → `orion_addr`; Step 4 / re-pair → `iphone_addr`. On reconnect, peer address determines role.

3. **Directed advertising once full.** Link layer only accepts the two stored addresses.

Factory reset zeroes both NVS entries and clears both NimBLE bond records.

### Advertising state machine

| Bond state | Advertising mode | Manufacturer-data flag |
|---|---|---|
| 0 bonded (fresh / post-factory-reset) | Public undirected | `0x01 SETUP` |
| 1 bonded — PC only, iPhone empty, **Orion disconnected** | Public undirected | `0x02 RUNTIME` |
| 1 bonded — PC only, iPhone empty, **Orion connected** | **Stopped** — nothing to advertise for (fresh iPhone only solicited from re-pair screen). Re-armed on Orion drop or re-pair window open. | (none) |
| 1 bonded — PC only, iPhone empty, **re-pair window open** | Public undirected (ANCS solicitation) | `0x02 RUNTIME` |
| 1 bonded — iPhone only | Public undirected | `0x01 SETUP` |
| 2 bonded, ≥1 disconnected | **Public undirected** (either bonded peer can reconnect). No accept-list — bonding is state-gated + all data chars encrypted, and iPhone's rotating RPAs make an accept-list fragile. | `0x02 RUNTIME` |
| 2 bonded, both connected | **Stopped**. Re-armed on either disconnect. | (none) |
| Runtime re-pair-iPhone in progress | Public undirected until iPhone re-bonds | `0x02 RUNTIME` |

### Advertising mode transitions

| Event | Action required |
|---|---|
| iPhone bond formed | Both slots full → once both connected, advertising stops; on either later disconnect, restart public undirected (`0x02 RUNTIME`) |
| Bonded peer reconnects (both bonded) | BLE auto-stops adv on connect → `onConnect` restarts undirected adv for the *other* peer; stops again once both connected |
| Bonded peer disconnects (both bonded) | `onDisconnect` restarts public undirected (`0x02 RUNTIME`) |
| iPhone unpaired (`on_unpair_phone`) | Delete iPhone bond + zero `iphone_addr`. Orion connected → advertising stops (nothing to solicit until re-pair screen opens); Orion disconnected → restart public undirected. **ANCS UUID is advertised only once the re-pair screen opens** (`set_iphone_pairing_window(true)`), not on unpair itself. |
| Orion bond formed (Step 2) | iPhone slot empty → public undirected (`0x01`→`0x02`). Once Orion connected with no re-pair window open, advertising stops (re-armed on drop or re-pair window). |
| Factory reset | Wipe both bonds + NVS → restart public undirected, `0x01 SETUP` |

### Advertising payload

- **Device name:** `Ori-XX-XX` (e.g. `Ori-XT-9F`).
- **Service UUIDs** — two mutually-exclusive flavours (two 128-bit UUIDs don't fit one 31-byte packet):
  - **Orion-discovery (default):** `Ori Sync Service` UUID + mode flag in the primary packet.
  - **iPhone-pairing** (Setup Step 4 or runtime re-pair, `set_iphone_pairing_window(true)`): the **ANCS UUID** (`7905F431-B5CE-4E99-A40F-4B1E122D00D0`) as a **Service Solicitation** (AD type `0x15`, "I want a device that *provides* ANCS") — **not** a provided-service list (`0x06`/`0x07`); advertising it as provided made Ori visible in nRF but iOS never engaged it. Plus a generic Appearance (`0x0180`). Built by hand via `addData()` (NimBLE has no solicitation setter). Ori Sync UUID/mode flag are dropped during this window. Outside active pairing, the ANCS solicitation is omitted.
  - Device name is in the scan response in both flavours. Ori never uses directed advertising (both-bonded reconnect is public undirected).
- **Manufacturer data:** byte 0 = `0xFF FF` (placeholder company ID); byte 1 = `0x01 SETUP` / `0x02 RUNTIME`.
- **Interval:** 100 ms for every public undirected state (setup, Orion-bonded/iPhone-empty runtime, both-bonded reconnect). Ori is wall-powered, so continuous fast advertising is fine; it stops entirely once both bonded peers are connected.

Orion uses the mode flag to detect "Ori factory-reset since last bond" without connecting (§7).

---

## 3. Service and characteristics

**One service, eighteen characteristics** — plus a separate BLE SIG standard service for firmware version (§3.1).

```
Ori Sync Service:  6F726900-0000-4F72-9F00-000000000000
```

Each characteristic UUID replaces bytes 4–5 of the base with the offset below.

| # | Name | UUID offset | Properties | Direction | Encrypted? |
|---|---|---|---|---|---|
| 1 | Device Status | `0001` | Read, Notify | Ori → Orion (notify) | No |
| 2 | Time Sync | `0002` | Write (response) | Orion → Ori | Yes |
| 3 | Profile Info | `0003` | Write (response) | Orion → Ori | Yes |
| 4 | Profile Photo | `0004` | Write, **Write NR** | Orion → Ori (chunked) | Yes |
| 5 | Meeting List | `0005` | Write, **Write NR** | Orion → Ori (chunked) | Yes |
| 6 | Time Off Entry | `0006` | Write, **Write NR** | Orion → Ori (chunked) | Yes |
| 7 | Sync Control | `0007` | Write, Notify | Orion ↔ Ori | Yes |
| 8 | **Device Command** | `0008` | Write (response) | Orion → Ori | Yes |
| 9 | Sync Manifest | `0009` | Write, Notify | Orion ↔ Ori | Yes |
| 10 | **Keyboard Command** | `000A` | Read, Notify | Ori → Orion (notify) | Yes |
| 11 | **Host Volume State** | `000B` | Read, Write (response) | Orion → Ori (+ Orion reads) | Yes |
| 12 | **Media Metadata** | `000C` | Write, Notify | Orion → Ori | Yes |
| 13 | **Media Album Art** | `000D` | Write (no response) | Orion → Ori (chunked) | Yes |
| 14 | **Device Settings** | `000E` | Read, Write (response), Notify | Orion → Ori (+ Orion reads/subscribes) | Yes |
| 15 | **Phone Bond Status** | `000F` | Read, Notify | Ori → Orion (notify) | Yes |
| 16 | **ANCS Notification** | `0010` | Read, Notify | Ori → Orion (notify) | Yes |
| 17 | **ANCS Call State** | `0011` | Read, Notify | Ori → Orion (notify) | Yes |
| 18 | **ANCS Notification Action** | `0012` | Write (response) | Orion → Ori | Yes |

Reads/writes on encrypted characteristics over an unencrypted link return `INSUFFICIENT_AUTHENTICATION`. Chars 16–18 are covered in full in §13.

**Every notify characteristic MUST also declare base `Read`** (chars 1, 10, 14, 15, 16, 17 all do), even though Orion only ever subscribes to 10/14/16/17. Hard Windows requirement: WinRT's GATT stack silently drops `ValueChanged` for a Notify-only characteristic — `subscribe()`/CCCD-write still report success, so the failure is invisible. Found 2026-07-11 when chars 10/16/17 (originally Notify+MITM only) had every notification silently dropped by WinRT before reaching Orion. Fix: add `Read` (kept MITM-gated via `READ_AUTHEN`). Never remove `Read` from a notify characteristic to "tighten" it. Char 14 (Device Settings) gained `Notify` on 2026-07-13, already satisfying this rule since it already declared base `Read`.

### 3.1 Device Information Service (BLE SIG standard — separate from Ori Sync Service)

Ori also exposes the standard **Device Information Service** (`0x180A`), with one characteristic:

| Characteristic | UUID | Properties | Encrypted? |
|---|---|---|---|
| Firmware Revision String | `0x2A26` | Read | No |

Plain UTF-8 semver string (e.g. `"1.0.0"`) — not CBOR, static for the connection's life (changes only after a USB CDC update reboot, `ota.md`), unencrypted so it's readable without bonding. How Orion learns the running firmware version — see §9.

---

## 4. Payload encoding — CBOR

All structured payloads use CBOR (RFC 8949). Libraries: Arduino — `ArduinoCBOR`; Rust (Orion) — `ciborium`/`serde_cbor`. Unknown keys are silently ignored (forward-compatible).

**Keys are single characters** — tiny, frequent payloads with no human reading raw CBOR off the wire (logs print decoded fields with full names), and it measurably cuts fragment count on the one repeated payload (Meeting List). Each schema below is commented with the full field name.

### Schemas

```cbor
ProfileInfo = {
  "n": text,           // name.  UTF-8, ≤ 32 chars (≤ 96 bytes wire)
  "t": text,           // title. UTF-8, ≤ 32 chars (≤ 96 bytes wire)
  "e": text,           // email. optional; UTF-8, ≤ 32 chars; absent or "" = not shown
  "p": text            // phone. optional; UTF-8, ≤ 16 chars; absent or "" = not shown
}

TimeSync = {
  "u": uint,           // epoch_utc. seconds since 1970-01-01 UTC
  "z": text,           // tz. POSIX TZ string (e.g. "PST8PDT"/"LOC-2"), NOT an IANA name —
                       // fed straight into newlib's setenv("TZ",tz)/tzset() (gatt_server.cpp
                       // apply_time_sync()), which silently falls back to UTC on an IANA
                       // string like "America/Los_Angeles". Orion converts from the local
                       // UTC offset — see _local_posix_tz() in tools/mock_orion_ble.py.
  "x": uint            // tx_ms. sender's monotonic ms at send time (round-trip correction)
}

Meeting = {
  "i": text,           // id. calendar-provider-stable id
  "s": uint,           // start. epoch UTC
  "e": uint,           // end. epoch UTC
  "t": text,           // title. ≤ 128 UTF-8 bytes — firmware truncates past that
  "l": text,           // loc. ≤ 64 UTF-8 bytes — firmware truncates past that
  "o": text            // org. ≤ 64 UTF-8 bytes — firmware truncates past that
}

MeetingList = {
  "d": uint,           // date. epoch UTC of local-midnight (day-rollover detection)
  "m": [Meeting, ...]  // items
}

TimeOffEntry = {
  "s": uint,           // start
  "e": uint,           // end
  "d": text,           // destination, e.g. "Lisbon, Portugal"
  "m": bytes           // image. JPEG (528×396 target); may be empty. Orion resizes before sending.
}

SyncControl = {
  "o": "BEGIN" | "END" | "ACK" | "NACK",  // op
  "s": uint,           // seq
  "r": text,           // reason. optional, populated for NACK
  "t": uint            // total. optional, BEGIN only — total application-payload bytes Orion
                        // will write across Time Sync + Profile/Photo/Meetings/Time Off (Time
                        // Sync unconditionally; the rest only the `needed` subset on a delta
                        // sync). Absent or 0 = indeterminate. Device Settings isn't counted here.
}

SyncManifest_Write = {           // Orion → Ori — Device Settings fields have no hash entry (written outside BEGIN/END)
  "p": bytes(32),      // profile_sha
  "h": bytes(32),      // photo_sha
  "m": bytes(32),      // meetings_sha
  "t": bytes(32)       // to_sha
}

SyncManifest_Notify = {          // Ori → Orion
  "n": [text, ...]               // needed. subset of {"profile","photo","meetings","to"}
}

KeyboardCommand = {              // Ori → Orion, notify
  "o": "vol_set" | "play_pause" | "prev" | "next" | "shortcut" | "seek",  // op
  "a": uint            // arg. vol_set → 0..100; shortcut → slot 1..3; seek → seconds
  // No dedicated `mute` op — mute is a user-configurable shortcut action in Orion settings.
}

HostVolumeState = {              // Orion → Ori, write (+ Orion can read it back)
  "l": uint,           // level. 0..100
  "m": bool            // mute
}

MediaMetadata = {                // Orion → Ori, write (+ Ori can notify on it)
  "t": text,           // title
  "a": text,           // artist
  "c": bool,           // can_seek. optional; absent = false. Ori hides scrubber when false.
  "p": bool,           // playing. optional; absent = no change. Orion MUST push this on every
                       // track change and every play/pause toggle so Ori's icon and local
                       // position timer stay in sync with the OS.
  "o": uint,           // position_s. optional; present only on track change/seek. Paired with "d".
  "d": uint            // duration_s. optional; paired with "o". Ori resets its dead-reckoning
                       // timer from "o"+"d"; between pushes it advances position_s on its own tick.
}

// Media Album Art — raw JPEG bytes (not CBOR), via §5 chunking. Orion resizes to 484×216.

DeviceSettings = {          // Orion → Ori, write (response). Fields optional; absent = unchanged.
                             // All present fields validated before any applied (atomic).
                             // Applied immediately, outside BEGIN/END (like Clock Face/Presence).
  "p": uint,     // presence. 0=Available 1=Busy 2=Away 3=Offline. Ephemeral (not in NVS) —
                 // Orion re-writes every (re)connect; Ori shows Offline before first write
                 // and immediately on BLE drop.
  "1": text,     // shortcut_slot1. icon token ≤ 19 chars (§12). Unknown token hides the slot.
  "2": text,     // shortcut_slot2.
  "3": text,     // shortcut_slot3. "1"/"2"/"3" persisted to NVS; write-only-on-change,
                 // read back on (re)connect.
  "c": uint,     // clock_face. 0=Digital 1=Analog. NVS-persisted, write-only-on-change.
  "h": uint,     // time_format. 0=24h (default) 1=12h. Governs every wall-clock display
                 // (status bar, both clock faces, meeting list, ANCS timestamps).
                 // NVS-persisted, write-only-on-change.
  "f": uint,     // ancs_filter. 0=Disabled 1=CallOnly (ANCS CategoryID 1 only)
                 // 2=Important (IncomingCall or ANCS Important flag) 3=All (default).
                 // NVS-persisted, write-only-on-change.
  "w": uint,     // weather_condition. 0=Clear 1=PartlyCloudy 2=Cloudy 3=Rain 4=Thunderstorm
                 // 5=Snow 6=Fog. Ephemeral; always sent together with "d"+"u".
  "d": int,      // temperature. Whole degrees in the unit "u" declares — Ori renders the raw
                 // integer verbatim, never converts. Signed (sub-zero). Ephemeral, with "w"+"u".
  "u": uint,     // temperature_unit. 0=Fahrenheit 1=Celsius. Ephemeral; a write with only
                 // some of "w"/"d"/"u" present is ignored — always send all three together.
  "k": uint      // seek_step_s. Double-tap-left/right-third-of-album-art seek step, in
                 // seconds (1-60, default 10). NVS-persisted, write-only-on-change,
                 // read back on (re)connect — media-mode.md.
}
// Out-of-range field value → NACK_CBOR_DECODE via SyncControl notify.
//
// Read (Orion → Ori): returns all NVS-persisted fields ("c","h","f","1","2","3","k") plus three
// read-only identity/link fields (an incoming write simply never looks for these keys —
// §4's "unknown keys ignored" gives them read-only semantics for free):
//   "s": serial_number, "b": manufacture_date (ISO-8601 "YYYY-MM-DD") — from the write-once
//     "factory" NVS partition (provisioning.md), untouched by nvs::factory_reset(). "" on an
//     unprovisioned unit.
//   "r": signal_bars, uint 0-4 — Ori's OWN live RSSI to Orion, bucketed on every read (same
//     ladder as PhoneBondStatus's "s"). Windows' btleplug can only read a peripheral's RSSI
//     from adverts, which stop once connected — Ori can read its live connection RSSI
//     directly (ble_gap_conn_rssi), so it reports its own reading back instead.
// Presence/weather not returned (ephemeral; Orion is source of truth). Orion reads on
// (re)connect to restore its settings UI without caching what it last wrote.

PhoneBondStatus = {            // Ori → Orion, notify + readable (CBOR)
  "b": bool,     // bonded.    true = iPhone NVS slot is occupied.
  "c": bool,     // connected. true = BLE link to iPhone is currently up.
  "n": text,     // name.      iPhone's GAP Device Name (e.g. "Xander's iPhone"), or ""
                 //            when not connected/read failed. ≤ 63 UTF-8 bytes.
  "d": text,     // device_type. The bonded phone's marketing model name — iPhone or iPad,
                 //              e.g. "iPhone 17 Pro Max" or "iPad Pro 12.9-inch (6th gen) —
                 //              Wi-Fi + Cellular". Device Information Service (0x180A / Model
                 //              Number String 0x2A24) actually returns Apple's internal
                 //              hardware identifier (e.g. "iPhone18,2" or "iPad14,6"), NOT a
                 //              marketing name — Ori resolves it via a compiled-in
                 //              identifier→name table (iphone_model_map.h's
                 //              iphone_model::resolve(), which covers both product lines)
                 //              before ever putting it on the wire, so Orion can just display
                 //              this string as-is, no resolution of its own. Entries that
                 //              would otherwise collapse to an identical name across sibling
                 //              identifiers (every iPad2,x is "iPad 2") carry a short
                 //              connectivity/radio/region suffix instead — "iPad 2 — Wi-Fi +
                 //              3G (GSM)" vs "(CDMA)" vs plain "iPad 2 — Wi-Fi" — which is why
                 //              this field is 63 bytes, not a short fixed name. Also how both
                 //              sides tell "iPhone" from "iPad" for UI text
                 //              (ancs_client::phone_kind_word() / app.js's phoneKindWord()) —
                 //              a plain prefix check, since every resolved/raw string already
                 //              starts with one or the other.
                 //              Falls back to the raw identifier itself (still ≤ 31 bytes,
                 //              e.g. "iPhone19,3") for a model newer than Ori's firmware
                 //              table — never blank just because it's unrecognized.
                 //              "" when not connected, the service isn't exposed, or the
                 //              read failed. Read once per connection (static for the link's
                 //              life, like "n"). Orion-side: unlike every other field in this
                 //              struct, "d" is write-through persisted to disk
                 //              (store::SavedState::phone_device_type, pc-app.md) — a device
                 //              model never changes for a given bond, so Orion keeps showing
                 //              the last-known value across an Ori disconnect/app restart
                 //              instead of blanking it, same treatment as Ori's own serial
                 //              number/manufacture date. Cleared only when the iPhone's bond
                 //              genuinely ends (unpair, or Ori's own factory reset taking the
                 //              iPhone bond down with it, §2).
  "m": uint,     // missed_calls.    live count of active ANCS notifications, CategoryID
                 //                  MissedCall (2), passing `ancs_filter` (Device Settings "f").
  "u": uint,     // unread_messages. live count, CategoryID Social (4) — ANCS' closest
                 //                  category to "message" apps — passing `ancs_filter`.
  "t": uint,     // total_notifications. live count of every OTHER category (never calls,
                 //                      always relayed via AncsCallState instead), passing
                 //                      `ancs_filter`, excluding whatever "m"/"u" already
                 //                      counted — the three counts are mutually exclusive.
  "s": uint,     // signal_bars. 0-4, bucketed from the live iPhone connection RSSI
                 //              (NimBLEClient::getRssi()) — a real per-connection reading,
                 //              since Ori holds this link directly.
  "l": uint      // battery_level. 0-100 (%), from the iPhone's Battery Service
                 //                (0x180F / Battery Level 0x2A19). Read once on connect,
                 //                then notify-driven (that characteristic supports Notify) —
                 //                no polling, updates the instant the phone reports a change.
                 //                That fresh connect-time read is forwarded into THIS
                 //                characteristic's own PhoneBondStatus push immediately
                 //                (ancs_client::on_iphone_connected() → ble_manager's
                 //                notify_phone_bond_status(..., battery_level) →
                 //                gatt_server.cpp) — not left for a later battery-change
                 //                notify or ANCS queue event to surface it. Without this, a
                 //                reconnect pushed "l":0 (this characteristic's own
                 //                disconnect-zeroed cache) until something else happened to
                 //                trigger a fresh stats push.
}
// "m"/"u"/"t" run through the SAME `ancs_filter` gate as the AncsNotification relay (§13's
// `passes_current_filter()`, not a second implementation) and Ori's own on-device tile
// counts/drill-down — every surface counts exactly what its list shows. A filter change
// immediately re-pushes PhoneBondStatus (`ancs_client::set_filter()` → `push_phone_stats()`),
// not just on the next queue event. "m"/"u"/"t"/"s"/"l" are always 0 when "c" is false ("d" is
// "" instead, matching "n"). Ori notifies on every iPhone state change and whenever counts/
// signal bucket/battery change while connected (queue change, filter change, RSSI poll, or
// battery poll) — not staged through §6.0's BEGIN/END. Orion reads on (re)connect to recover
// initial state without waiting for a notify.

AncsNotification = {          // Ori → Orion, notify (char 0010) — see §13
  "o": "add" | "remove" | "clear",  // op. "add" covers a genuinely-new notification AND an
                          // ANCS Modified event (Orion replaces its stored copy in place,
                          // keyed by "u"). "remove" carries only "u". "clear" wipes the
                          // entire local mirror (firmware doesn't track which uids it
                          // previously relayed — on a filter change it just sends "clear"
                          // then re-"add"s everything passing the new filter).
  "u": uint,              // uid. ANCS notification UID — stable identity for row/detail
                          // keying and the Action target (char 0012). Ignored for "clear".
  "k": text,              // icon_token. "add" only. Same vocabulary as firmware's
                          // ancs_icons.h (`firmware.md`). "" / unrecognised → category fallback.
  "c": uint,              // category. "add" only. AncsCategory, 0-12 (§13).
  "a": text,              // app. "add" only. Display name, e.g. "Gmail".
  "t": text,              // title. "add" only. Sender / notification title.
  "b": text,              // body. "add" only. Message preview, ≤ 512 UTF-8 bytes — matches
                          // Ori's own on-device storage (§10) and the max ANCS is asked for
                          // (ancs_client.cpp's request_attributes()). Delivered chunked
                          // since this alone exceeds one ATT notification (§5). Full content
                          // beyond this (rare) is always available by checking the phone.
  "e": uint,              // recv_epoch. "add" only. Unix epoch received (0 = unknown) —
                          // Orion derives "X min ago" from this, never a pre-formatted string.
  "p": text,              // pos_label. "add" only. ANCS PositiveActionLabel; "" = none
                          // (never fabricated by Orion).
  "n": text,              // neg_label. "add" only. ANCS NegativeActionLabel.
  "g": bool,              // has_neg_action. "add" only. EventFlags NEGATIVE_ACTION bit.
  "s": bool               // silent. "add" only. EventFlags SILENT bit.
}

AncsCallState = {             // Ori → Orion, notify (char 0011) — see §13
  "st": uint,   // state. 0 = none/ended, 1 = ringing, 2 = active.
  "u": uint,    // uid. the call's ANCS UID (0 when st == 0).
  "e": uint,    // elapsed_s. only meaningful when st == 2 — seconds since answered, so
                // Orion's timer resumes correctly after a reconnect mid-call.
  "a": text,    // app. caller-identity display_name, e.g. "Phone". "" for st==0. Same wire
                // caps as AncsNotification's "a"/"t"/"p"/"n".
  "t": text,    // title. caller name/number.
  "p": text,    // pos_label. ANCS PositiveActionLabel; "" once active (iOS drops the answer
                // action once picked up — how Ori itself tells ringing from on-call).
  "n": text,    // neg_label. ANCS NegativeActionLabel (Decline / Hang Up / End, verbatim).
  "g": bool,    // has_neg_action.
  "k": text     // icon_token. calling app's icon token, same vocabulary as
                // AncsNotification's "k", populated for st==1/2, "" for st==0 — the ONLY
                // source of a call's icon (calls carry no char-0010 payload).
}

AncsNotificationAction = {    // Orion → Ori, write (response) (char 0012) — see §13
  "u": uint,    // uid. target notification (or call) UID.
  "a": uint     // action. 0 = Positive, 1 = Negative. 0 → answer_notification(). 1 → the
                // SAME dismiss-vs-drop choice the on-device swipe makes: dismiss_notification()
                // (send ANCS Negative) only if the notification has a negative action, else
                // drop_notification() (local removal, no ANCS write — sending a Negative to a
                // notification with none can make the phone re-assert it). NACK_CBOR_DECODE
                // if "u" is no longer live.
}
```

### Device Status (single-byte enum, no CBOR)

```
0x00 SETUP_WAITING_PAIRING       — Step 2, before bond
0x01 SETUP_BONDED_AWAITING_SYNC  — Step 2 done, before first sync
0x02 SETUP_SYNCING               — Step 3 (Orioning)
0x03 SETUP_SYNC_COMPLETE         — advances Ori UI to Step 4
0x10 RUNTIME_READY
0x11 RUNTIME_RECONNECTING        — bonded reconnect in progress
0x12 RUNTIME_SYNCING             — periodic refresh while RUNTIME_READY
0xF0 ERROR_GENERIC
```

(`0x20` reserved — do not reuse. OTA screen is driven locally via USB CDC; see `ota.md`.)

### Device Command (4-byte payload, NOT CBOR)

Magic-routed — the 4-byte value determines the action:

```
0xFA 0xC7 0x5E 0x5E    // Factory Reset  — wipe both bonds + NVS, reboot into first-boot setup
0x55 0x4E 0x50 0x52    // Unpair Phone   — wipe iPhone bond only, show re-pair screen on Ori
0x52 0x53 0x59 0x4E    // Resync ANCS ("RSYN") — replay the full ANCS relay state to Orion:
                       //   AncsNotification{op:"clear"} + one {op:"add"} per currently-queued
                       //   notification passing the filter (char 0010), plus the current
                       //   AncsCallState (char 0011). Orion writes this once per (re)connect,
                       //   after its notify pipeline is fully subscribed and draining — the
                       //   PULL-based trigger that makes §13's resync race-free by construction.
```

Any other value → `NACK_BAD_MAGIC` via SyncControl notify.

---

## 5. Chunking protocol

Used by Profile Photo, Meeting List, Time Off Entry, and Media Album Art (Orion→Ori writes). AncsNotification (Ori→Orion notify) also chunks — see "AncsNotification chunking" below — as a simpler variant of this format (no NACK/retry, no windowed flow control).

### MTU strategy

- Request **247 bytes** on connect. Fall back to **23 bytes** if refused.
- Per-fragment payload at MTU 247: `247 - 3 (ATT header) - 6 (frame header) = 238 bytes`.

### Frame format

```
Offset  Size  Field
0       2     seq_num     (uint16, little-endian) — 0-based fragment index
2       2     total_frags (uint16, little-endian)
4       2     payload_len (uint16, little-endian)
6       N     payload
```

### Reassembly

- Accumulate in order. When `seq_num == total_frags - 1`, concatenate and decode.
- Gap → `SyncControl{op:"NACK", seq:<expected>, reason:"chunk_missing"}`; sender restarts from seq=0.
- Timeout (10 s no progress) → NACK `"chunk_timeout"`.

### Write type and flow control (throughput)

Profile Photo, Meeting List, and Time Off Entry advertise **both** `Write` and `Write Without Response`; Media Album Art is Write-No-Response only. Orion SHOULD stream fragments Write-No-Response — it avoids the per-fragment ATT round-trip, letting many fragments ride each connection event (especially on the 2M PHY). This is the dominant throughput lever (≈5–10× over per-fragment Write-with-response).

Because Write-No-Response has no ATT-level pacing, Orion MUST bound how far it runs ahead of Ori so a fast burst can't overrun Ori's RX buffers:

- **Windowed checkpoint (required).** Send fragments Write-No-Response, but every **`WINDOW` fragments (≤ 32, ≈ 7.6 KB)** — and on the final fragment — send that one as a **Write-with-response**. Its ATT ack confirms every prior fragment landed and blocks the sender until Ori has drained them, keeping in-flight data ≤ one window. No new ops/characteristics needed — the same fragment frame is just written with-response at the checkpoint.
- Reliability is unchanged: the link layer still acks/retransmits every packet, and a dropped-on-overrun fragment is caught by the existing seq-gap → `NACK_CHUNK_MISSING` → restart. The window just avoids overrun (and the wasteful full-item restart) in the first place.
- Control/command characteristics (Time Sync, Sync Control, Factory Reset, Sync Manifest) stay **Write-with-response** — tiny, no speed benefit, and the ack/error code is wanted.

### AncsNotification chunking (char 0010 — the one Ori→Orion direction)

Once AncsNotification's field caps were raised to match Ori's own on-device storage (§10), a maxed-out `body` no longer fits one ATT notification, so `notify_chunked()` (`gatt_server.cpp`) sends every op on this characteristic — `add`/`remove`/`clear` alike — through the same frame format (even `remove`/`clear`, which always fit `total_frags:1`), so Orion's reassembler never has to guess whether a notify is bare CBOR or a chunk frame.

Same frame format as §5 (seq_num/total_frags/payload_len, uint16 LE), sized from the negotiated MTU. Deliberately simpler than the write-direction protocol, since this is a one-way relay with no `SyncControl`-equivalent NACK channel and no overrun risk **for a single notification**:

- **No NACK/retry, at the protocol level.** A multi-fragment `add`'s fragments are sent back-to-back from Ori's single main task (nothing else can interleave), and the link layer itself acks/retransmits every packet. If Orion's reassembler sees a gap or `total_frags` mismatch, it discards the partial buffer and waits for the next `seq_num == 0` — the next queue mutation on Ori re-sends anyway, so a dropped sequence self-heals without an explicit retry. This reasoning still holds for one notification at a time: at most ~3-4 fragments for a maxed-out body — nowhere near the burst sizes §5's `WINDOW` protects against (built for a 512 KB Time Off photo).
- **The resync burst is the one case that DOES need pacing — a transport-level fix, not a protocol change.** `ancs_client::resync_orion_relay()` (fired from `onSubscribe()` on char 0010, `firmware.md`) replays every currently-queued notification as a fresh `add`, back-to-back, once per (re)connect. Found on hardware 2026-07: with several notifications queued (a captured log showed 10, some spanning up to 8 fragments each) fired with zero pacing — often while the connection was still mid-MTU-negotiation — NimBLE's outgoing mbuf pool (default 12×256 B in NimBLE-Arduino's `nimconfig.h`) filled up, most `notify()` calls failed (`BLE_HS_ENOMEM`), and Orion tore down the link (`BLE_HS_HCI_ERR 0x13`, "Remote User Terminated Connection") rather than receive a stalled/garbled relay stream. Fixed with three changes, all firmware-internal (no wire-format change):
  - `resync_orion_relay()` yields a short pace (`ANCS_RESYNC_ITEM_PACING_MS`, 15 ms) **between queued items**, not between one item's own fragments — the single-notification case above is untouched.
  - `notify_chunked()` (`gatt_server.cpp`) retries a failed fragment a bounded number of times with a short backoff (`ANCS_NOTIFY_MAX_RETRIES`/`ANCS_NOTIFY_RETRY_BACKOFF_MS`) before giving up and logging — scoped to this transient mbuf-pool-full case, not a general per-fragment ack/retry protocol (NimBLE's `notify()` only returns a bool, so this can't branch on ENOMEM specifically; retrying any failure is the closest available proxy).
  - The mbuf pool itself was also widened (`CONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT=24` in `platformio.ini`) as defense in depth — the pacing fix is what actually corrects the burst; the wider pool just gives more headroom while it drains.

---

## 6. Connection sequences

### 6.0 Sync staging and progress

For **every** sync session (initial pairing or reconnect delta), Ori stages incoming items in PSRAM as they arrive and commits them to NVS **atomically at `SyncControl{op:"END"}`** — mirroring the USB CDC OTA pathway (`ota.md`), which stages the firmware image before a single flash commit.

- Between `BEGIN` and `END`, nothing is written to NVS and no cached state changes. If the link drops mid-session, Ori discards the staged data and keeps serving previously cached data.
- At `END`, Ori parses each staged item, computes its SHA-256, and updates the live UI in one batch. **Profile, Profile Photo, and Time Off Entry persist to NVS.**
- **Time Sync and Meeting List are RAM-only on Ori** — staged and applied at `END` but NOT written to NVS. Neither has a manifest hash; Orion always resends Time Sync unconditionally. A power cycle drops both; the next sync repopulates them. See `meeting-list.md` for rationale.
- **Shortcut Config** rides the **Device Settings** characteristic (char `000E`) outside the BEGIN/END window — applied immediately, like Clock Face/ANCS Filter. Persisted to NVS on Ori (no manifest hash); Orion reads it back on (re)connect, writes only on user change.
- **Display blackout during flash write.** Ori blanks the framebuffer before NVS writes (`hardware.md`) — only when the commit touches NVS (Profile, Photo, Time Off); RAM-only commits skip it.
- **Progress.** If `BEGIN` includes `total` (> 0), Ori tracks cumulative bytes written and derives `pct = received * 100 / total` (capped at 99 until `END`, then 100). Drives the Step 2/3 Orioning ring and the runtime "Refreshing your day" overlay — shown only when `total` > `RECONNECT_OVERLAY_MIN_BYTES` (200 B), see `state-machine.md`. Absent/0 `total` still stages correctly, just without a byte-driven ring.

### 6.1 First-time pairing

```
Ori boots fresh → public undirected adv, mode=0x01 SETUP
Orion scans → finds Ori-XT-9F → user taps "Pair"
Orion connects → ATT MTU exchange → BLE bonding (LE SC, Passkey Entry)
  • Ori displays the 6-digit code (Step 2 passkey modal)
  • User confirms on Orion → LTK stored on both sides
  • Ori holds the peer address PROVISIONALLY (RAM only) — the orion_addr NVS
    slot is NOT written yet (see handshake note below)

Ori notifies Device Status = SETUP_BONDED_AWAITING_SYNC
Orion writes Device Settings (shortcuts + clock face if non-default)
  → Applied immediately on Ori — outside the BEGIN/END staging pipeline.
Orion computes total = byte-length of (Time Sync + Profile Info
                                        + Profile Photo + Meeting List + Time Off Entry) payloads
Orion writes SyncControl{op:"BEGIN", seq:1, total:total}
  → HANDSHAKE: a valid SyncControl{BEGIN} on the encrypted link is Ori's proof
    that the bonded peer is the Orion app (not a phone or other PC someone paired
    off the passkey screen). On this BEGIN, Ori commits the provisional address
    to the orion_addr slot. A peer that bonds but never sends a valid BEGIN
    within ~5 s is disconnected and its LTK bond deleted — never saved as Orion.
Orion writes Time Sync
Orion writes Profile Info
Orion writes Profile Photo (chunked)
Orion writes Meeting List (chunked)
Orion writes Time Off Entry (chunked)
Orion writes SyncControl{op:"END", seq:1}

Ori commits all staged items to NVS + computes per-item SHA-256 hashes (§6.0).
Ori notifies Device Status = SETUP_SYNC_COMPLETE
  → Ori UI advances to Step 4 (phone pairing, optional)
```

### 6.2 Bonded reconnect — hash-manifest delta sync

```
Ori boots OR loses connection → public undirected adv (§2)
Orion sees adv → reconnects → encrypted via stored LTK

Ori notifies Device Status = RUNTIME_RECONNECTING
  → Reconnecting overlay appears on left panel

Orion writes Device Settings (shortcuts — always; clock face / ANCS filter if changed)
  → Applied immediately on Ori — outside the BEGIN/END staging pipeline.
Orion writes Presence Status (via Device Settings "p" field)
  → Also applied immediately.
Orion writes Weather (via Device Settings "w"/"d"/"u" fields) — always, all three together
  → Also applied immediately. Ori was showing no weather icon (or a stale one
    it can no longer trust) while disconnected — this repopulates it.
Orion writes Sync Manifest: { profile_sha, photo_sha, meetings_sha, to_sha }
Ori compares against NVS/RAM hashes → notifies Sync Manifest: { needed: [...] }

Orion computes total = byte-length of (Time Sync + only the `needed` items)
Orion writes SyncControl{op:"BEGIN", seq:N, total:total}
Orion writes Time Sync (always — ~20 bytes, clock may drift) — INSIDE the
  BEGIN/END session, like every other staged item (§6.0/§6.3); this is what
  both implementations (Orion's run_sync, tools/mock_orion_ble.py) have
  always done. (An earlier revision of this sequence showed Time Sync before
  BEGIN; firmware tolerates that order too — gatt_server.cpp's stage_begin()
  carries a just-staged pre-BEGIN Time Sync into the new session instead of
  wiping it — but inside the session is the canonical order.)
Orion writes only requested items (profile → photo → meetings → to)
Orion writes SyncControl{op:"END", seq:N}

Ori commits all staged items to NVS + hashes atomically (§6.0).
Ori notifies Device Status = RUNTIME_READY → overlay dismissed.
```

**Hash content:** SHA-256 of canonical deterministic CBOR (sorted keys, smallest-encoding ints). If hashes drift for any reason, next reconnect re-pushes automatically.

### 6.3 Periodic refresh (while RUNTIME_READY)

| Trigger | Cadence | Effect |
|---|---|---|
| Time Sync | Every 10 min | Write Time Sync (inside BEGIN/END) |
| Meeting List | Calendar event or every 15 min | Hash-check via Manifest, push if needed |
| Time Off Entry | Calendar event | Hash-check, push if needed |
| Profile Info / Photo | User edit in Orion | Hash-check, push if needed |
| Shortcut Config | User changes slot | Write Device Settings {"1","2","3"} outside BEGIN/END; read back on (re)connect |
| Presence Status | Teams change or ~60 s poll | Write Device Settings {"p"} (only when value changes) |
| Weather | Weather-API poll, ~15–30 min | Write Device Settings {"w","d","u"} together (only when any value changes) |
| Clock Face / Time Format / ANCS Filter | User changes setting | Write Device Settings {"c"}, {"h"}, or {"f"} |
| Seek Step | User changes setting | Write Device Settings {"k"} outside BEGIN/END; read back on (re)connect |

Periodic refreshes set `RUNTIME_SYNCING` briefly but do **not** trigger the reconnecting overlay.

### 6.4 Device Settings push (no manifest, no hash)

Device Settings (char `000E`) is outside the BEGIN/END pipeline. Each field is ephemeral or user-configured:

- **Presence** (`"p"`): ephemeral. Orion writes on every (re)connect and every Teams state change. Ori shows `Offline` before the first write and immediately on BLE drop.
- **Weather** (`"w"`/`"d"`/`"u"`): ephemeral, same treatment as Presence — all three written together, on (re)connect and whenever the poll (~15–30 min) detects an actual change. Before the first write, and on BLE drop, Ori hides the weather icon/temperature entirely (no placeholder) — same "don't show what can't be verified" policy as Presence-offline.
- **Shortcuts** (`"1"/"2"/"3"`): NVS-persisted, write-only-on-change. Orion reads back on (re)connect to populate its Quick Actions UI.
- **Clock Face** (`"c"`): NVS-persisted, write-only-on-change — not on every reconnect.
- **Time Format** (`"h"`): NVS-persisted, write-only-on-change. 0=24-hour (default), 1=12-hour; governs every wall-clock display on Ori.
- **ANCS Filter** (`"f"`): NVS-persisted, write-only-on-change.
- **Seek Step** (`"k"`): NVS-persisted, write-only-on-change. 1-60 seconds, default 10; how far double-tapping the left/right third of the album art seeks — `media-mode.md`.

Orion can write any subset of fields in one Device Settings write (e.g. presence-only on a Teams change).

**Read on (re)connect:** Orion reads Device Settings once per connection to recover the seven NVS-persisted fields (`"c"`, `"h"`, `"f"`, `"1"`/`"2"`/`"3"`, `"k"`), so its settings UI shows correct values without caching what it last wrote. Presence is not returned (ephemeral; Orion is the source of truth).

**Notify (added 2026-07-13):** Ori also notifies the full Device Settings read-response payload whenever its own live `signal_bars` (`"r"`) bucket to Orion changes — sampled every 5 s while connected, notified only on an actual bucket change (0-4), mirroring Phone Bond Status's identical RSSI-poll-and-notify-on-change treatment for the iPhone link. This is the only field the notify exists for: the NVS-persisted fields above only ever change via an Orion write in the first place, so Orion already knows their new value the moment it sends one and gains nothing from being told again; `"s"`/`"b"` (serial/manufacture date) never change post-provisioning. Orion's Ori Info/Stats modal (`pc-app.md`) subscribes to this notify to show live signal bars while open, replacing an earlier fixed-interval poll on Orion's own side.

---

## 7. Disconnect, reconnect, and cache semantics

- Profile, photo, and Time Off persist to NVS on `SyncControl{op:"END"}` and show a "SYNCED · X min ago" pill while offline. **Meeting List and local time are RAM-only** (§6.0) — the pill appears only on a *runtime* disconnect, never after a power cycle (list is empty → "No meetings today"). See `meeting-list.md`.
- Factory reset wipes NVS and both bonds; next connection is first-time pairing.

### 7.1 Factory-reset-during-reconnect

**Ori side:** drops both bonds, wipes NVS, reboots into setup with flag `0x01 SETUP`.

**Orion side:**
1. **Adv-flag check (preferred):** adv flag `0x01 SETUP` → Orion deletes its bond without connecting. UI: "Ori has been factory reset. Re-pair."
2. **Encryption-failure fallback:** LTK mismatch → Orion deletes bond and treats as path 1.

In both cases: **stop the reconnect loop** until the user re-pairs.

### 7.2 Remote factory reset (Orion → Ori)

1. Orion shows confirm dialog.
2. User confirms → Orion writes magic `0xFA C7 5E 5E` over encrypted link.
3. Ori wipes NVS + both bonds → reboots into first-boot setup.
4. Orion sees disconnect → deletes its bond → prompts re-pair.

Local long-press-photo reset is identical; both paths converge in the same firmware routine.

---

## 8. Errors

| Code | Meaning |
|---|---|
| `NACK_CHUNK_MISSING` | Reassembly gap; sender restarts from seq=0 |
| `NACK_CHUNK_TIMEOUT` | Reassembly stalled; sender restarts |
| `NACK_CBOR_DECODE` | Payload malformed; sender re-encodes, retries once |
| `NACK_TOO_LARGE` | Payload exceeds cap |
| `NACK_BAD_MAGIC` | Device Command magic not recognised |

Link-layer encryption failure (`BLE_HS_ENC_FAIL`) signals a stale bond — see §7.1. Surface it cleanly; do not retry-loop.

---

## 9. Versioning

No wire-level protocol version negotiation or compatibility gate — Ori and Orion are developed and released together, and this contract is expected to stay stable. Unknown CBOR keys are always silently ignored (§4), so additive changes are non-breaking by construction; revisit this section only if a genuinely breaking change is ever needed.

`fw_version` (semver, e.g. `"1.2.3"`) is read from the standard **Firmware Revision String** characteristic (§3.1) and drives exactly one thing: Orion polls `ori.app` for the latest release and offers a user-initiated "Install update" in Settings when newer exists (`ota.md`). Purely optional; never blocks or gates the sync flow.

---

## 10. Caps and limits

| Item | Limit |
|---|---|
| `ProfileInfo.name` | ≤ 32 chars (Orion input cap, `pc-app.md`); ≤ 96 UTF-8 bytes wire |
| `ProfileInfo.title` | ≤ 32 chars; ≤ 96 UTF-8 bytes wire |
| `ProfileInfo.email` | ≤ 32 chars; optional |
| `ProfileInfo.phone` | ≤ 16 chars; optional |
| Profile Photo (JPEG, 228×228) | hard cap 200 KB |
| Meeting `title` | ≤ 128 UTF-8 bytes (firmware buffer; truncated on a UTF-8 boundary past that). Orion should cap before sending. |
| Meeting `loc` | ≤ 64 UTF-8 bytes (firmware buffer; truncated past that) |
| Meeting `org` | ≤ 64 UTF-8 bytes (firmware buffer; truncated past that) |
| Meeting list total | ≤ 32 meetings/day |
| `TimeOffEntry.destination` | ≤ 48 UTF-8 bytes |
| Time Off image (JPEG, 528×396) | hard cap 512 KB — Orion resizes to 528×396 before sending |
| `MediaMetadata.title` | ≤ 192 UTF-8 bytes |
| `MediaMetadata.artist` | ≤ 96 UTF-8 bytes |
| Media Album Art (JPEG, 484×216) | target 15–30 KB; hard cap 64 KB |
| `DeviceSettings.presence` | uint 0–3 |
| `DeviceSettings.slot1/2/3` | ≤ 19 chars each (firmware buffer) — icon token, e.g. "vol-mute" |
| `DeviceSettings.clock_face` | uint 0–1 |
| `DeviceSettings.time_format` | uint 0–1 |
| `DeviceSettings.ancs_filter` | uint 0–3 |
| `DeviceSettings.weather_condition` | uint 0–6 |
| `DeviceSettings.temperature` | int −40…140 (unit declared by `"u"` — see §4) |
| `DeviceSettings.temperature_unit` | uint 0–1 |
| `DeviceSettings.seek_step_s` | uint 1–60 (default 10) |
| `DeviceSettings.serial_number` | ≤ 32 chars (firmware `g_serial[32]`, `factory_info.cpp`) — read-only, provisioning.md |
| `DeviceSettings.manufacture_date` | ≤ 16 chars, ISO-8601 "YYYY-MM-DD" (firmware `g_mfg[16]`) — read-only, provisioning.md |
| `DeviceSettings.signal_bars` | uint 0–4 — read-only, live |
| `PhoneBondStatus.name` | ≤ 63 UTF-8 bytes (firmware `g_phone_name[64]` minus null terminator) |
| `PhoneBondStatus.device_type` | ≤ 63 UTF-8 bytes (firmware `g_phone_device_type[64]` minus null terminator) — widened from 32 so `iphone_model_map.h`'s connectivity/region suffixes fit (e.g. "iPad Pro 12.9-inch (5th gen) — Wi-Fi + Cellular (Global)", the longest current entry at 58 bytes) |
| `PhoneBondStatus.missed_calls/unread_messages/total_notifications` | uint 0–255 (capped at `MAX_ANCS_NOTIFICATIONS` = 100 in practice) |
| `PhoneBondStatus.signal_bars` | uint 0–4 |
| `PhoneBondStatus.battery_level` | uint 0–100 |
| Unpair Phone Command | exactly 4 bytes (0x55 0x4E 0x50 0x52) |
| `AncsNotification.icon_token` | ≤ 31 UTF-8 bytes — matches Ori's own on-device storage (`app_state.cpp`'s `AncsDetailEntry::token[32]`); longest known firmware token, `microsoft_authenticator`, is 23 |
| `AncsNotification.app` | ≤ 39 UTF-8 bytes — matches `AncsDetailEntry::display_name[40]` |
| `AncsNotification.title` | ≤ 192 UTF-8 bytes, truncated on a UTF-8 boundary past that — matches `AncsDetailEntry::title[193]` |
| `AncsNotification.body` | ≤ 512 UTF-8 bytes, truncated on a UTF-8 boundary past that — matches `AncsDetailEntry::body[513]`, itself matching the 512-byte max Ori asks ANCS for (`ancs_client.cpp`'s `request_attributes()`, AttributeID 0x03). Delivered chunked (§5) since this alone exceeds one ATT notification |
| `AncsNotification.pos_label` / `neg_label` | ≤ 32 UTF-8 bytes each — matches `AncsDetailEntry::pos_label[33]`/`neg_label[33]` (real ANCS labels are short standard strings, rarely approached) |
| `AncsNotification.category` | uint 0–12 |
| `AncsCallState.state` | uint 0–2 |
| `AncsCallState.icon_token` | ≤ 24 UTF-8 bytes — char 0011 is NOT chunked (calls carry no body), so it keeps the original tighter size, same vocabulary as `AncsNotification.icon_token` |
| `AncsNotificationAction.action` | uint 0–1 |

`AncsNotification`'s caps are sized so a worst-case `"add"` (every optional field at max length) still fits one unchunked 238-byte fragment. If real-world CBOR encoding of max-length content ever measures over budget, tighten these caps further rather than adding chunking to a third notify-only characteristic.

---

## 11. Implementation owners

- **`esp32-connectivity`** — GATT server, bonding, chunk reassembly, NVS persistence + hashes, factory-reset routine, ANCS client, chars 10–18. No HOGP. Owns the filter-gated relay logic (§13) — the SAME filter evaluation used for the on-device status bar, not a second implementation.
- **`orion-sync`** — scanning + connection lifecycle, bonding storage, hash-manifest delta, chunked writes, background keep-alive, USB CDC OTA path (`ota.md`), media-mode OS bridge (§12), ANCS relay (§13). Reads char 15 on connect and subscribes to notifies; writes Unpair Phone magic via char 8 on user request; writes char 14 on reconnect (shortcuts+presence+weather), on Teams presence changes, weather-poll changes, and clock-face/time-format/ANCS-filter changes. Subscribes to chars 16–17, maintains Orion's local notification mirror; writes char 18 on Answer/Decline/End-call/Dismiss taps. Owns bringing the Orion window to the foreground on `AncsCallState{st:1}` (`orion-frontend` owns the in-app UI that follows).

Pre-release: no need to bump a version header per change — just keep this file in sync with the firmware/Orion implementations as the contract evolves.

---

## 12. Media-mode bridging — the Orion-mediated model

Ori uses chars 10–13 instead of HOGP. Orion bridges each `KeyboardCommand` notify to OS APIs and mirrors OS state changes back to Ori.

Orion is one Tauri v2 (Rust) codebase covering this contract on both platforms — see `memory.md`. The table below lists both OS columns since the native APIs differ per platform (behind `#[cfg(target_os = ...)]`); the macOS column is forward documentation, not yet implemented or verified.

### Command flow — Ori → Orion → OS

| User action | `KeyboardCommand` | Windows | macOS (planned) |
|---|---|---|---|
| Tap art | `{op:"play_pause"}` | `SendInput VK_MEDIA_PLAY_PAUSE` | `CGEventCreateMediaKeyEvent NX_KEYTYPE_PLAY` ¹ |
| Swipe right | `{op:"next"}` | `SendInput VK_MEDIA_NEXT_TRACK` | `NX_KEYTYPE_NEXT` ¹ |
| Swipe left | `{op:"prev"}` | `SendInput VK_MEDIA_PREV_TRACK` | `NX_KEYTYPE_PREVIOUS` ¹ |
| Vertical swipe release | `{op:"vol_set", arg:N}` | `IAudioEndpointVolume::SetMasterVolumeLevelScalar(N/100.0)` → write back `HostVolumeState` | `AudioObjectSetPropertyData kAudioHardwareServiceDeviceProperty_VirtualMainVolume` → write back |
| Tap shortcut slot N | `{op:"shortcut", arg:N}` | Orion runs configured action — see supported tokens below | same |

¹ Requires macOS Accessibility permission, granted on first launch; Controls-mode features stay inert until granted.

Supported shortcut actions (configured per slot in Orion settings): `vol-mute`, `mic-mute`, `screenshot`, `lock-screen`, `favorite-1`/`favorite-2`/`favorite-3` (three independently-configured custom actions, each its own keyboard combo — `pc-app.md`), `calculator`, `copy`/`cut`/`paste`/`undo`/`redo`/`save` (Ctrl+C/X/V/Z/Y/S, replayed via the same key-injection path as a recorded Favorite). No dedicated `mute` op — mute is the `vol-mute` shortcut.

### Shortcut icon assignment — Orion → Ori

Which icon shows in each slot is configured in Orion's settings UI and delivered via `"1"/"2"/"3"` of **Device Settings** — outside BEGIN/END, applied immediately, **persisted to NVS on Ori** (no manifest hash). Orion reads current values back on every (re)connect, writes only on change. Ori maps each token to a compiled-in asset (`shortcut_icons.h`); unknown tokens hide the slot's button rather than failing the write. Adding a new icon *type* still requires a firmware update — Device Settings only carries which existing compiled-in icon each slot shows.

### State push flow — OS → Orion → Ori

- **Volume change:** `IAudioEndpointVolumeCallback` (Win) / `AudioObjectAddPropertyListener` (macOS, planned) → debounce ~100 ms → write `HostVolumeState`
- **Track change:** `GlobalSystemMediaTransportControlsSessionManager` (Win) / `MRMediaRemoteRegisterForNowPlayingNotifications` (macOS, planned — private `MediaRemote` framework, viable since Orion ships direct-download notarized, never via the Mac App Store; re-verify per OS version) → write `MediaMetadata` + resize art to 484×216 JPEG (target 15–30 KB) → chunk-write `MediaAlbumArt`
  - **A new track change aborts an in-flight art transfer immediately** rather than queuing behind it: Orion runs each `MediaAlbumArt` chunk stream as its own task and cancels the previous one when a newer track's art is requested (`spawn_album_art_push` in `central.rs`), so skipping rapidly through a playlist doesn't make each stale image stream to completion before the current one starts. On the wire the old stream simply stops mid-fragment; Ori discards the half-received image the moment the new transfer's `seq==0` frame lands (its chunked reassembler resets on `seq 0` — `chunked_transfer.cpp`), so no explicit "cancel" op is needed and no partial image is ever shown.

### Swipe-vs-push race (vertical volume swipe)

While a vertical swipe is active (≥ 25 px threshold): Ori ignores incoming `HostVolumeState` writes. On lift: override drops, but Ori delays accepting the next push for ~800 ms (HUD linger). Orion debounces its OS volume push by ~100 ms.

### Cache + reconnect semantics

- `HostVolumeState` is read on (re)connect so the next swipe starts from the correct level.
- `MediaMetadata` and `MediaAlbumArt` are PSRAM-cached (not NVS); on reconnect Orion re-pushes the current track, or writes `MediaMetadata{title:"", artist:""}` if nothing is playing.

---

## 13. ANCS relay to Orion

Orion's iPhone Info modal drill-down (missed calls / messages / other notifications, tap-to-read, incoming-call banner) needs individual notification content and live call state — none of which chars 1–15 carry (`PhoneBondStatus` is aggregate counts only). Chars 16–18 close that gap. `esp32-connectivity` owns the firmware side; `orion-sync` owns the BLE central + local mirror; `orion-frontend` owns the UI those events drive.

### Filter gates the relay, not just the display

Every notification Ori tracks is gated by the **same** `ancs_filter` level (Device Settings `"f"`, §3/§6.4) that already governs the on-device status bar, evaluated once centrally. **`Disabled` means Orion receives nothing from chars 16–17 at all** — no `add`, no call-state transitions, not even for an incoming call — so Orion's "bring window to front on ringing" needs no filter check of its own; the triggering notify simply never arrives. `CallOnly`/`Important`/`All` narrow or widen which notifications qualify, identically to the status bar.

**Filter changes re-evaluate live state**, not just future events: on a filter change, Ori sends `{op:"clear"}` then `{op:"add"}` for every currently-queued notification passing the new filter — a full clear-and-repopulate, not a diff, so firmware never has to track which uids it previously relayed. Orion's mirror always reflects the current filter, never a stale connect-time snapshot.

### Notification lifecycle

- `{op:"add"}` fires for a notification that passes the filter and hasn't already been relayed, or that HAS been relayed but iOS just sent a Modified event for (Orion replaces its stored copy in place, keyed by `"u"`).
- `{op:"remove"}` fires whenever a previously-relayed notification leaves Ori's queue for any reason (user dismiss, phone clear, ANCS Removed, FIFO eviction past `MAX_ANCS_NOTIFICATIONS`). Never sent for a notification that was never relayed.
- **Orion must close a stale open detail.** If the notification shown in Orion's detail modal is removed, Orion closes it and returns to the drill-down list — the content no longer exists on the phone.
- Stacking (same app+title grouped into one row) is display-only on both sides — the wire always carries individual notifications.

### Call takeover

`AncsCallState{st:1}` (ringing) is Orion's cue to raise itself (show, focus, un-minimize) and present the incoming-call view, so the user notices even if Orion was backgrounded. `{st:2}` (active — e.g. reconnect mid-call) resumes the in-call view with its timer seeded from `"e"`, not restarted at zero. `{st:0}` closes whatever call view is open (declined/ended from the phone, or iPhone link dropping).

### Actions

Orion writes `AncsNotificationAction` only in direct response to a user tap (Answer/Decline/End call/Dismiss/Read all — one write per UID for a "Read all" on a stacked group). Ori performs the identical action an on-device tap would trigger — no separate "remote action" code path, just a second caller — including the same **dismiss-vs-drop** choice for a Negative (`"a":1`): `dismiss_notification()` only when the notification has a negative action, else `drop_notification()` (local removal, no ANCS write). The **removal relay back to Orion is emitted from `queue_remove()` itself** — one choke-point firing `{op:"remove"}` (normal) or `AncsCallState{st:0}` (call) regardless of which path removed it — never from the phone's own Removed-event handler, which runs after a local dismiss has already dropped the category and would misfire. Orion does **not** update its UI optimistically — it waits for the resulting notify, same as every other state change in this protocol.

### Chunked, matching Ori's own storage

`AncsNotification`'s field caps (§10) match Ori's own on-device storage exactly — Orion never shows less body text than Ori itself does, and `body` matches the 512-byte max Ori requests from ANCS. A maxed-out `body` alone exceeds one fragment, so `"add"` is sent chunked (§5); `"remove"`/`"clear"` go through the same always-framed path for consistency (a single `total_frags:1` frame), so Orion's reassembler never has to guess whether a notify is bare CBOR or a chunk frame. Full content is still always available by checking the phone regardless — this is a generous cap, not an unlimited one.

### Icon tokens

`"k"` reuses firmware's per-bundle icon token vocabulary (`ancs_icons.h`, `firmware.md`'s token list) — Orion needs matching icon assets keyed by the same tokens (or a category fallback glyph) to stay visually consistent with the device. Adding a new brand icon is a firmware change on Ori's side plus an asset addition on Orion's — the same two-sided update the device's own icon set already requires.
