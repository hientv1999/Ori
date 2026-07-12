# Ori — BLE GATT Protocol Specification

**Contract revision:** 1.0 — pre-release; this document isn't finalized, no version-history tracking needed yet. Documentation-only label — there is no wire-transmitted protocol version negotiation; see §9.
**Status:** Authoritative — `esp32-connectivity` (Ori firmware) and `orion-sync` (Orion app) must conform.

This document defines the single BLE GATT contract between Ori and the Orion PC app.

**Out of scope for BLE:** ANCS phone link (`connectivity.md`); firmware updates run over USB CDC (`ota.md`).

---

## 1. Roles

| Side | Role |
|---|---|
| **Ori (Arduino on ESP32-S3)** | GATT server + Advertiser. Hosts every characteristic below. |
| **Orion** — one Tauri v2 (Rust) codebase, both Windows and macOS (`memory.md`) | GATT client via `btleplug`. The only side that can issue Read/Write requests. |

LE Secure Connections with Passkey Entry (6-digit numeric, MITM-protected) is mandatory. After first pairing the device is bonded; subsequent reconnects are silent. On Windows, Orion owns the passkey-entry UI (custom digit-box modal driving WinRT's `DeviceInformationCustomPairing`) rather than the OS's default pairing flyout — see §6.1 and `memory.md`'s pairing-UX decision. macOS has no equivalent app-level pairing hook and is deferred until that build starts.

---

## 2. Advertising and bond policy

Ori accepts **at most two bonded peers**: one PC (Orion) and one iPhone (ANCS). New bonds are accepted only in the state-gated pairing windows below; outside them an unknown device that connects can neither bond (pairing is rejected) nor read/write any data characteristic (all are encrypted), so it gains nothing. When both slots are full and both peers are connected, Ori stops advertising; if a bonded peer drops, Ori advertises **public undirected** so it can reconnect on its own.

### Bond slot enforcement

1. **State-machine-gated pairing.** New bonds accepted only during:

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

   Step 2 bond → `orion_addr`; Step 4 / re-pair → `iphone_addr`. On reconnect, peer address determines role (Orion sync vs. ANCS).

3. **Directed advertising once full.** Link layer only accepts the two stored addresses.

Factory reset zeroes both NVS entries and clears both NimBLE bond records.

### Advertising state machine

| Bond state | Advertising mode | Manufacturer-data flag |
|---|---|---|
| 0 bonded (fresh / post-factory-reset) | Public undirected | `0x01 SETUP` |
| 1 bonded — PC only, iPhone slot empty, **Orion disconnected** | Public undirected (so Orion can reconnect) | `0x02 RUNTIME` |
| 1 bonded — PC only, iPhone slot empty, **Orion connected** | **Advertising stopped** — nothing to advertise for (Orion is here; a fresh iPhone is only solicited from the re-pair screen). Re-armed when Orion drops (`onDisconnect`) or the re-pair window opens (`set_iphone_pairing_window(true)`). | (none) |
| 1 bonded — PC only, iPhone slot empty, **re-pair window open** | Public undirected (ANCS solicitation, see payload) | `0x02 RUNTIME` |
| 1 bonded — iPhone only | Public undirected | `0x01 SETUP` |
| 2 bonded — PC + iPhone, ≥1 disconnected | **Public undirected** (so *either* bonded peer can reconnect on its own — iPhone off the advert, Orion via its own scan). No accept-list (bonding is state-gated + all data chars encrypted; iPhone uses rotating RPAs which make an accept-list fragile). | `0x02 RUNTIME` |
| 2 bonded — PC + iPhone, both connected | **Advertising stopped** (nothing to advertise for). Re-armed on either disconnect. | (none) |
| Runtime re-pair-iPhone in progress | Public undirected until iPhone re-bonds | `0x02 RUNTIME` |

### Advertising mode transitions

| Event | Action required |
|---|---|
| iPhone bond formed | Both slots full → once both peers are connected, advertising stops; if either later disconnects, restart **public undirected** (`0x02 RUNTIME`) so it can reconnect |
| Bonded peer reconnects (both bonded) | BLE auto-stops adv on connect → `onConnect` restarts undirected adv so the *other* bonded peer can also reconnect; stops again once both are connected |
| Bonded peer disconnects (both bonded) | `onDisconnect` restarts **public undirected** (`0x02 RUNTIME`) so the dropped peer can reconnect |
| iPhone unpaired (`on_unpair_phone`) | Delete iPhone bond + zero `iphone_addr`. If Orion is **connected**, **advertising stops** — nothing to solicit until the user opens the re-pair screen; if Orion is **disconnected**, restart **public undirected** (`0x02 RUNTIME`) so it can reconnect. The **ANCS UUID is advertised only once the user opens the re-pair screen** (`set_iphone_pairing_window(true)`), not immediately on unpair — see Advertising payload below. |
| Orion bond formed (Step 2) | iPhone slot still empty → public undirected (`0x01 SETUP` → `0x02 RUNTIME`). Once Orion is **connected** with no re-pair window open, **advertising stops** (re-armed when Orion drops, or when the re-pair window opens). |
| Factory reset | Wipe both bonds + NVS addresses → restart public undirected, flag `0x01 SETUP` |

### Advertising payload

- **Device name:** `Ori-XX-XX` (per-device suffix, e.g. `Ori-XT-9F`)
- **Service UUIDs:** the public undirected advert has two mutually-exclusive flavours (two 128-bit UUIDs don't fit one 31-byte packet):
  - **Orion-discovery (default):** `Ori Sync Service` UUID + manufacturer mode flag in the **primary advertising packet**, so Orion can discover/classify Ori.
  - **iPhone-pairing (pairing screen on-screen + iPhone slot empty — Setup Step 4 or runtime re-pair, `set_iphone_pairing_window(true)`):** the **ANCS UUID** (`7905F431-B5CE-4E99-A40F-4B1E122D00D0`) goes in the **primary advertising packet** as a **Service Solicitation** (AD type `0x15`, "list of 128-bit Service Solicitation UUIDs") — meaning "I want a device that *provides* ANCS" — **not** as a provided-service list (`0x06`/`0x07`). Ori consumes ANCS; the solicitation is what makes iOS engage it. (Advertising ANCS as a *provided* service made the device visible in nRF — "Services: ANCS" — but iOS never listed/engaged it.) Plus a generic **Appearance** (`0x0180`). Built by hand via `addData()` (NimBLE has no solicitation setter). The Ori Sync UUID / mode flag are dropped during this window (Orion is connected through Step 4 / re-pair). In normal runtime (iPhone slot empty, not actively pairing) the ANCS solicitation is omitted, so Ori does not solicit iPhone connections.
  - Device name is in the scan response in both flavours. Ori no longer uses directed advertising — see §2 (both-bonded reconnect is public undirected).
- **Manufacturer data:** byte 0 = `0xFF FF` (placeholder company ID); byte 1 = `0x01 SETUP` / `0x02 RUNTIME`
- **Interval:** 100 ms for all public undirected advertising — the 0-bonded setup state, the Orion-bonded/iPhone-slot-empty runtime state, and the both-bonded reconnect state (≥1 peer disconnected). Fast advertising keeps both reconnection and iPhone (ANCS) discovery snappy. (Ori is wall-powered, so continuous fast advertising is fine; advertising stops entirely once both bonded peers are connected.)

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
| 14 | **Device Settings** | `000E` | Read, Write (response) | Orion → Ori (+ Orion reads) | Yes |
| 15 | **Phone Bond Status** | `000F` | Read, Notify | Ori → Orion (notify) | Yes |
| 16 | **ANCS Notification** | `0010` | Read, Notify | Ori → Orion (notify) | Yes |
| 17 | **ANCS Call State** | `0011` | Read, Notify | Ori → Orion (notify) | Yes |
| 18 | **ANCS Notification Action** | `0012` | Write (response) | Orion → Ori | Yes |

Reads/writes on encrypted characteristics over an unencrypted link return `INSUFFICIENT_AUTHENTICATION`. Chars 16–18 are covered in full in §13.

**Every notify characteristic MUST also declare the base `Read` property (chars 1, 10, 15, 16, 17 above all do), even though Orion only ever subscribes and never reads chars 10/16/17.** This is a hard Windows requirement, not a style choice: the WinRT GATT stack silently refuses to raise `ValueChanged` (i.e. delivers no notifications) for a characteristic that lacks `Read` — `subscribe()` still returns success and the CCCD write still ACKs, so the failure is invisible except that notifications never arrive. Found 2026-07-11: chars 10/16/17 were originally `Notify` + MITM only, and every notification Ori sent on them (confirmed leaving the firmware, `rc=0`) was dropped by WinRT before reaching Orion — the "PhoneBondStatus count is right but the notification list is empty" bug (char 15 has `Read`, so its count survived via the connect-time read). Adding `Read` (kept MITM-gated via READ_AUTHEN; the readable value is just the last-notified payload) fixed delivery. Do not remove `Read` from a notify characteristic to "tighten" it.

### 3.1 Device Information Service (BLE SIG standard — separate from Ori Sync Service)

Ori also exposes the standard **Device Information Service** (`0x180A`), a distinct GATT service (not part of the Ori Sync Service above), with one characteristic:

| Characteristic | UUID | Properties | Encrypted? |
|---|---|---|---|
| Firmware Revision String | `0x2A26` | Read | No |

Value is a plain UTF-8 semver string (e.g. `"1.0.0"`) — not CBOR, static for the life of the connection (only changes across a reboot after a USB CDC firmware update, `ota.md`), and unencrypted so it's readable without bonding. This is how Orion learns the running firmware version — see §9.

---

## 4. Payload encoding — CBOR

All structured payloads use CBOR (RFC 8949). Libraries: Arduino — `ArduinoCBOR`; Rust (Orion) — `ciborium` or `serde_cbor`. Unknown keys are silently ignored (forward-compatible).

**Keys are single characters** — these are tiny, frequent payloads with no human ever reading raw CBOR off the wire (firmware/mock-tool logs print the *decoded* fields with full names), so there's no readability cost, and on the one chunked/repeated payload (Meeting List) it measurably cuts fragment count. Each schema below is commented with the full field name on every line.

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
  "z": text,           // tz. POSIX TZ string, e.g. "PST8PDT" or a fixed-offset
                       // form like "LOC-2" — Ori feeds this straight into
                       // newlib's setenv("TZ", tz)/tzset() (gatt_server.cpp
                       // apply_time_sync()), which does NOT understand IANA
                       // names ("America/Los_Angeles" silently falls back to
                       // UTC). Orion must convert the local UTC offset to a
                       // POSIX TZ string before sending — see
                       // tools/mock_orion_ble.py's _local_posix_tz() for a
                       // reference implementation.
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
                        // will write across Time Sync, Profile Info, Profile Photo, Meeting List,
                        // and Time Off Entry (Time Sync unconditionally; the rest only the `needed`
                        // subset on a delta sync). Absent or 0 = indeterminate.
                        // Device Settings is outside the BEGIN/END pipeline and not counted here.
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
  "p": bool,           // playing. optional; absent = no change. True = playing, false = paused.
                       // Orion MUST push this on every track change and every play/pause toggle
                       // so Ori's icon and local position timer stay in sync with the OS.
  "o": uint,           // position_s. optional; present only on track change or seek. Seconds
                       // from the start of the track. Always paired with "d".
  "d": uint            // duration_s. optional; paired with "o". Total track duration in seconds.
                       // Ori uses "o"+"d" to reset its dead-reckoning position timer; between
                       // pushes Ori advances position_s on its own 1-second tick.
}

// Media Album Art — raw JPEG bytes (not CBOR), via §5 chunking. Orion resizes to 484×216.

DeviceSettings = {             // Orion → Ori, write (response). All fields optional —
                               // absent keys leave the current state unchanged. All present
                               // fields are validated before any are applied (atomic).
                               // Applied immediately, outside the BEGIN/END staging pipeline
                               // (same as Clock Face / Presence individually — see §6.4).
  "p": uint,           // presence. 0=Available 1=Busy 2=Away 3=Offline. Not persisted to NVS;
                       // Orion re-writes on every (re)connect. Fallback to Offline on BLE drop.
  "1": text,           // shortcut_slot1. icon token ≤ 19 chars — see §12 for supported tokens.
  "2": text,           // shortcut_slot2. Unknown tokens hide the slot button in the UI.
  "3": text,           // shortcut_slot3. Persisted to NVS; Orion writes only when the user
                       // changes a slot. Orion reads back on (re)connect (same policy as "c"/"f").
  "c": uint,           // clock_face. 0=Digital 1=Analog. Persisted to NVS; survives power cycles.
                       // Orion writes only when the user changes the setting.
  "h": uint,           // time_format. 0=24-hour (default) 1=12-hour. Governs every wall-clock
                       // display (status bar, digital + analog clock, meeting list times, ANCS
                       // notification timestamps). 12-hour renders "2:30 PM"; 24-hour "14:30".
                       // Persisted to NVS; Orion writes only when the user changes the setting
                       // (same write-only-on-change + read-back policy as "c").
  "f": uint,           // ancs_filter. 0=Disabled 1=CallOnly 2=Important 3=All (default).
                       // Persisted to NVS; Orion writes only when the user changes the setting.
                       // Filter levels: Disabled=no notifications; CallOnly=ANCS CategoryID 1
                       // only; Important=IncomingCall OR ANCS Important flag; All=all events.
  "w": uint,           // weather_condition. 0=Clear 1=PartlyCloudy 2=Cloudy 3=Rain
                       // 4=Thunderstorm 5=Snow 6=Fog. Not persisted to NVS — ephemeral like
                       // presence. Always sent together with "d" and "u" (never a subset).
  "d": int,            // temperature. Whole-number degrees in whichever unit "u" declares —
                       // Orion picks the unit (e.g. per the user's locale/setting) and Ori just
                       // renders the raw integer + the unit letter ("72°F"/"22°C"), never
                       // converts between units. Signed to allow sub-zero readings. Not
                       // persisted to NVS. Always sent together with "w" and "u".
  "u": uint            // temperature_unit. 0=Fahrenheit 1=Celsius. Not persisted to NVS —
                       // ephemeral like "w"/"d", and always sent together with them (a write
                       // with only some of "w"/"d"/"u" present is ignored — ble-protocol.md §6.4).
}
// Any field value outside its valid range → NACK_CBOR_DECODE via SyncControl notify.
//
// Read (Orion → Ori): returns all NVS-persisted fields, plus three read-only
// device-identity/link fields Ori adds and never accepts on a write (an
// incoming write simply never looks for these keys — §4's "unknown keys
// ignored" policy gives them read-only semantics for free, no extra
// validation needed):
//   {"c": <clock_face>, "h": <time_format>, "f": <ancs_filter>,
//    "1": <slot1>, "2": <slot2>, "3": <slot3>,
//    "s": <serial_number>, "b": <manufacture_date>, "r": <signal_bars>}
// Presence and weather are not returned (both ephemeral; Orion is source of truth).
// Orion reads on (re)connect to restore its settings UI without having to cache what it last wrote.
//
// "s"/"b"/"r" back Orion's Ori Info modal (pc-app.md) — piggybacked on this
// characteristic rather than a dedicated one:
//   "s": serial_number text, "b": manufacture_date text (ISO-8601
//     "YYYY-MM-DD") — both read from Ori's separate write-once "factory" NVS
//     partition (provisioning.md), NOT the "ori" namespace nvs::factory_reset()
//     wipes. Empty string on an unprovisioned dev unit.
//   "r": signal_bars, uint 0-4 — Ori's OWN link to Orion, sampled live and
//     bucketed on every read (same 0-4 ladder as PhoneBondStatus's "s"
//     field). This is the reverse direction of that field: there Ori is
//     central for the iPhone link and can read its RSSI directly; here
//     Orion is central for the Ori link, and Windows' btleplug can only read
//     a peripheral's RSSI from advertising packets — which stop the moment
//     Ori is connected — so Orion can't read its own link's RSSI locally.
//     Ori can (ble_gap_conn_rssi works on either side of any of its active
//     connections), so it reports its own reading back instead of Orion
//     needing a workaround it fundamentally can't have on Windows.

PhoneBondStatus = {            // Ori → Orion, notify + readable (CBOR)
  "b": bool,     // bonded.    true = iPhone NVS slot is occupied (a bond exists).
  "c": bool,     // connected. true = BLE link to iPhone is currently up.
  "n": text,     // name.      iPhone's GAP Device Name (e.g. "Xander's iPhone"),
                 //            or "" when not connected / read failed. ≤ 63 UTF-8 bytes.
  "m": uint,     // missed_calls.    live count of active ANCS notifications with
                 //                  CategoryID MissedCall (2) that currently pass
                 //                  `ancs_filter` (Device Settings "f", §6.4).
  "u": uint,     // unread_messages. live count of active ANCS notifications with
                 //                  CategoryID Social (4) — ANCS' closest category
                 //                  to "message" apps (Messages/WhatsApp/etc. all
                 //                  report Social) — that currently pass `ancs_filter`.
  "t": uint,     // total_notifications. live count of active ANCS notifications
                 //                      in every OTHER category (excluding calls,
                 //                      which never count here — see below) that
                 //                      currently pass `ancs_filter` — excludes
                 //                      whatever's already counted in "m"/"u" so
                 //                      the three counts are mutually exclusive
                 //                      (a missed call isn't double-counted under
                 //                      the bell/notifications badge too).
  "s": uint      // signal_bars. 0-4, bucketed from the live iPhone connection RSSI
                 //              (NimBLEClient::getRssi() / ble_gap_conn_rssi — a
                 //              real per-connection reading, not a stale scan-time
                 //              value, since Ori holds this link directly).
}
// "m"/"u"/"t" are filtered through the SAME `ancs_filter` gate as the
// AncsNotification relay (chars 16–17, §13's `passes_current_filter()` —
// not a second implementation), and exclude calls entirely (INCOMING_CALL /
// ACTIVE_CALL never counted here — they relay exclusively via AncsCallState,
// same exclusion the relay and the on-device drill-down apply). This keeps
// Orion's badge counts and its own drill-down list (fed by that same relay)
// always in agreement, and means a filter change (Device Settings "f") is
// immediately followed by an updated PhoneBondStatus notify
// (`ancs_client::set_filter()` calls `push_phone_stats()`), not just on the
// next queue event. Ori's own on-device tile counts and drill-down list
// (`ancs_client::phone_stats()` / `list_bucket_groups()`) apply the SAME
// gate — every surface counts exactly what its list shows (policy changed
// 2026-07-11; the on-device drill-down previously bypassed the filter).
// "m"/"u"/"t"/"s" are always 0 when "c" is false — nothing left to verify once
// the link drops (same policy as Presence/Weather, §6.4). Ori notifies Orion
// on every iPhone state change (bond formed, connected, disconnected) AND
// whenever the notification counts or signal bucket change while connected
// (ancs_client, on every ANCS queue change, filter change, or a 5 s RSSI poll)
// — the stats fields are not staged through §6.0's BEGIN/END pipeline, same
// as the rest of this characteristic. Orion reads on (re)connect to recover
// initial state without waiting for a notify. Value is always kept current;
// the stored characteristic value equals the last-notified state.

AncsNotification = {          // Ori → Orion, notify (char 0010) — see §13
  "o": "add" | "remove" | "clear",  // op. "add" covers both a genuinely-new
                          // notification AND an ANCS Modified event (Orion
                          // replaces its stored copy in place, keyed by "u");
                          // "remove" carries only "u" — every other field is
                          // absent. "clear" carries no other fields at all —
                          // wipe the entire local mirror (see §13's filter-
                          // change handling: firmware doesn't track which
                          // uids it previously relayed, it just sends
                          // "clear" then re-sends "add" for everything that
                          // currently passes the new filter).
  "u": uint,              // uid. ANCS notification UID — stable identity used
                          // for row/detail keying and as the Action target
                          // (char 0012). Absent/ignored for "clear".
  "k": text,              // icon_token. "add" only. Same token vocabulary as
                          // firmware's ancs_icons.h (`firmware.md`'s list) —
                          // Orion maps it to the identical icon Ori's own status
                          // bar shows. "" / unrecognised → category fallback glyph.
  "c": uint,              // category. "add" only. AncsCategory, 0-12 (§13).
  "a": text,              // app. "add" only. Display name, e.g. "Gmail".
  "t": text,              // title. "add" only. Sender / notification title.
  "b": text,              // body. "add" only. Message preview, capped at 512
                          // UTF-8 bytes — matches Ori's own on-device storage
                          // (§10), which itself matches the max ANCS is asked
                          // for (ancs_client.cpp's request_attributes()) —
                          // nothing left on the table between phone, Ori, and
                          // Orion. Delivered chunked since that exceeds one
                          // ATT notification (§5's "AncsNotification
                          // chunking"). Ori's own on-device overlay has no
                          // display line cap of its own (LV_LABEL_LONG_WRAP,
                          // fully wraps whatever it's given); full content
                          // beyond this cap (rare — ANCS itself won't offer
                          // more than what was requested) is always available
                          // by checking the phone.
  "e": uint,              // recv_epoch. "add" only. Unix epoch the notification
                          // arrived (0 = unknown) — Orion derives its own
                          // "X min ago" from this rather than a pre-formatted string.
  "p": text,              // pos_label. "add" only. ANCS PositiveActionLabel;
                          // "" = no positive action (never fabricated by Orion —
                          // ble-protocol.md's existing "don't hardcode" rule for
                          // action button text applies here at the source).
  "n": text,              // neg_label. "add" only. ANCS NegativeActionLabel.
  "g": bool,              // has_neg_action. "add" only. EventFlags NEGATIVE_ACTION bit.
  "s": bool               // silent. "add" only. EventFlags SILENT bit.
}

AncsCallState = {             // Ori → Orion, notify (char 0011) — see §13
  "st": uint,   // state. 0 = none/ended, 1 = ringing, 2 = active.
  "u": uint,    // uid. the call's ANCS UID (0 when st == 0).
  "e": uint,    // elapsed_s. only meaningful when st == 2 — seconds since answered,
                // so Orion's timer resumes correctly after a reconnect mid-call
                // instead of restarting at 00:00.
  "a": text,    // app. caller-identity fields, populated for st==1/2, "" for st==0
                // (nothing to show once a call ends) — same wire caps as
                // AncsNotification's "a"/"t"/"p"/"n" (§10; a caller name is
                // functionally a notification title, same size budget).
                // display_name, e.g. "Phone".
  "t": text,    // title. caller name/number.
  "p": text,    // pos_label. ANCS PositiveActionLabel; "" once active (iOS drops
                // the answer action once a call is picked up — that transition
                // is how Ori itself tells "ringing" from "on call").
  "n": text,    // neg_label. ANCS NegativeActionLabel (Decline while ringing,
                // Hang Up / End once active — whatever iOS actually labelled it,
                // never fabricated by Orion).
  "g": bool,    // has_neg_action.
  "k": text     // icon_token. calling app's icon token, SAME vocabulary as
                // AncsNotification's "k" (ancs_icons.h / §13's icon list),
                // populated for st==1/2, "" for st==0. Lets Orion render the
                // real calling-app icon (Viber/Phone/…) for the call — in its
                // incoming/in-call view AND its header call chip — instead of a
                // generic call glyph; "" / unrecognised → category call glyph
                // fallback, same as AncsNotification. Calls carry no char-0010
                // payload (§13), so this is the ONLY source of a call's icon.
}

AncsNotificationAction = {    // Orion → Ori, write (response) (char 0012) — see §13
  "u": uint,    // uid. target notification (or call) UID.
  "a": uint     // action. 0 = Positive, 1 = Negative — ANCS PerformNotificationAction.
                // 0 → ancs_client::answer_notification(). 1 → the SAME choice
                // Ori's own on-device swipe makes (modal_ancs_list.cpp's
                // commit_row_delete): dismiss_notification() (send the ANCS
                // Negative) when the notification HAS a negative action, else
                // drop_notification() (remove from Ori's queue locally, no
                // ANCS action). Sending a Negative to a notification that has
                // none is a no-op the phone may answer by re-asserting the
                // notification (Modified/Added), which bounces the queue +
                // PhoneBondStatus count straight back — so a plain "clear this
                // from my list" must NOT hit the phone. NACK_CBOR_DECODE (via
                // SyncControl notify, reused) if "u" is no longer live.
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
                       //   PULL-based trigger that makes the §13 resync race-free by
                       //   construction (see §13 "Resync on (re)connect").
```

Any other value → `NACK_BAD_MAGIC` via SyncControl notify.

---

## 5. Chunking protocol

Used by Profile Photo, Meeting List, Time Off Entry, and Media Album Art (all
Orion→Ori writes). AncsNotification (Ori→Orion notify) also chunks — see
"AncsNotification chunking" below — but as a deliberately simpler variant of
this same frame format, not a full implementation of everything in this
section (no NACK/retry, no windowed flow control).

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

Profile Photo, Meeting List, and Time Off Entry advertise **both** `Write` and
`Write Without Response`; Media Album Art is `Write Without
Response` only. Orion SHOULD stream fragments **Write-No-Response** — it
avoids the per-fragment ATT round-trip, so many fragments ride each connection
event (especially on the 2M PHY) instead of one-per-event. This is the dominant
throughput lever (≈5–10× over per-fragment Write-with-response).

Because Write-No-Response has no ATT-level pacing, Orion MUST bound how far
it runs ahead of Ori so a fast burst can't overrun Ori's RX buffers:

- **Windowed checkpoint (required).** Send fragments Write-No-Response, but every
  **`WINDOW` fragments (≤ 32, ≈ 7.6 KB)** — and on the final fragment — send that
  one fragment as a **Write-with-response**. Its ATT acknowledgement confirms
  every prior fragment landed and blocks the sender until Ori has drained them,
  keeping in-flight data ≤ one window. No new ops or characteristics are needed —
  the same fragment frame is just written with-response at the checkpoint.
- Reliability is unchanged: the link layer still acks/retransmits every packet
  (no on-air loss or corruption), and a dropped-on-overrun fragment is caught by
  the existing seq-gap → `NACK_CHUNK_MISSING` → restart. The window makes overrun
  (and the wasteful full-item restart) not happen in the first place.
- Control/command characteristics (Time Sync, Sync Control, Factory Reset, Sync
  Manifest) stay **Write-with-response** — they are tiny (no speed benefit) and
  the ack/error code is wanted.

### AncsNotification chunking (char 0010 — the one Ori→Orion direction)

Superseded §13's original "No chunking" design (kept there historically as a
now-outdated rationale — small fixed field caps sized to fit one ATT
notification) once AncsNotification's field caps were raised to match Ori's
own on-device storage (§10), which made single-fragment delivery impossible
for a maxed-out `body`. `notify_chunked()` (`gatt_server.cpp`) sends every
op on this characteristic — `"add"`, `"remove"`, `"clear"` alike — through
this format, even though remove/clear always fit in one frame
(`total_frags:1`): a single always-framed format means Orion's reassembler
never has to guess whether a given notify is a bare CBOR payload or a chunk
frame.

Same frame format as above (seq_num/total_frags/payload_len, uint16 LE),
frame size derived from the connection's actual negotiated MTU (same
adaptive approach as `frag_size_for_mtu`), clamped to the same ~238-byte
convention. Deliberately simpler than the write-direction protocol in two
ways, both because this is a one-way relay with no equivalent of
`SyncControl` to carry a NACK back to Ori, and no equivalent overrun risk:

- **No NACK/retry.** A `remove`/`clear`/single-fragment `add` is tiny and
  self-contained; a multi-fragment `add`'s fragments are sent back-to-back
  from Ori's single main task (nothing else can interleave a notify on this
  characteristic mid-sequence), and the BLE link layer itself acks/retransmits
  every packet (no on-air loss). If Orion's reassembler ever sees a gap
  (unexpected `seq_num`) or a `total_frags` mismatch mid-sequence, it just
  discards the partial buffer and waits for the next `seq_num == 0` to
  resync — the next queue mutation on Ori re-sends the notification anyway
  (`ancs_client.cpp`'s existing add/remove/clear triggers), so a dropped
  sequence self-heals on its own without an explicit retry request.
- **No windowed flow control.** A handful of fragments (at most ~3-4 for a
  maxed-out body) is nowhere near the burst sizes §5's `WINDOW` protects
  against (built for a 512 KB Time Off photo or similar); back-to-back
  `notify()` calls at this scale stay well within the BLE stack's mbuf pool
  capacity.

---

## 6. Connection sequences

### 6.0 Sync staging and progress

For **every** sync session (initial pairing or reconnect delta), Ori stages incoming
items in PSRAM as they arrive and commits them to NVS **atomically at `SyncControl{op:"END"}`**
— mirroring the USB CDC OTA pathway (`ota.md`), which stages the firmware image in
PSRAM before a single flash commit.

- Between `BEGIN` and `END`, nothing is written to NVS and no cached state (profile
  card, meeting list, Time Off, hashes) changes. If the link drops mid-session, Ori discards
  the staged data and keeps serving the previously cached data — a partial transfer
  can never corrupt or partially overwrite NVS.
- At `END`, Ori parses each staged item, computes its SHA-256, and updates the live UI
  — in one batch. **Profile, Profile Photo, and Time Off Entry persist to NVS.**
- **Time Sync and Meeting List are RAM-only on Ori** — staged and applied at `END` but NOT written to NVS. Neither has a manifest hash; Orion always resends Time Sync unconditionally. A power cycle drops both; the next sync repopulates them. Orion sends Meeting List identically on the wire — the RAM-only choice is Ori's (no `proto` bump). See `meeting-list.md` for rationale.
- **Shortcut Config is no longer part of the staged sync pipeline.** It is delivered
  via the **Device Settings** characteristic (char `000E`) written outside the
  BEGIN/END window — applied immediately on write, like Clock Face / ANCS Filter.
  **Persisted to NVS on Ori** (no manifest hash). Orion reads Device Settings on
  every (re)connect to recover the current slot tokens; it only writes them when
  the user changes a slot in Orion's UI (same write-only-on-change policy as `"c"`/`"f"`).
- **Display blackout during flash write.** Ori blanks the framebuffer before NVS writes (`hardware.md` — LCD_CAM DMA vs. cache). Only when the commit touches NVS (Profile, Photo, Time Off); RAM-only commits (Time Sync, Meeting List) skip it entirely.
- **Progress.** If `BEGIN` includes `total` (> 0), Ori tracks cumulative bytes across all writes (chunked: `payload_len` per fragment, not the 6-byte header) and derives `pct = received * 100 / total` (capped at 99 until `END` commits, then 100). Drives both the Step 2/3 Orioning ring and the runtime "Refreshing your day" overlay (same `OrioningProgress` event). Overlay only shown when `total` > `RECONNECT_OVERLAY_MIN_BYTES` (200 B) — see `state-machine.md`. If `total` is absent or 0, staging still works but the ring is not byte-driven.

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
Orion writes Time Sync (always — ~20 bytes, clock may drift)
Orion writes Sync Manifest: { profile_sha, photo_sha, meetings_sha, to_sha }
Ori compares against NVS/RAM hashes → notifies Sync Manifest: { needed: [...] }

Orion computes total = byte-length of (Time Sync + only the `needed` items)
Orion writes SyncControl{op:"BEGIN", seq:N, total:total}
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

Periodic refreshes set `RUNTIME_SYNCING` briefly but do **not** trigger the reconnecting overlay.

### 6.4 Device Settings push (no manifest, no hash)

Device Settings (char `000E`) is outside the BEGIN/END pipeline. Each field is ephemeral or user-configured:

- **Presence** (`"p"`): ephemeral. Orion writes on every (re)connect and on every Teams state change. Before the first write, Ori displays `Offline`. On BLE link drop, Ori immediately renders `Offline` — stale presence would lie about reality.
- **Weather** (`"w"`/`"d"`/`"u"`): ephemeral, same treatment as Presence. Orion writes all three fields together on every (re)connect and whenever its weather-API poll detects a change (~15–30 min cadence — only write when the condition, temperature, or unit actually differs from the last push, not on every poll tick). Before the first write, and on BLE link drop, Ori hides the weather icon and temperature text entirely (no neutral/placeholder glyph) — a cached reading can't be verified as current, same "don't show what you can't verify" policy as Presence-offline and the status-bar clock before a time source connects.
- **Shortcuts** (`"1"/"2"/"3"`): persisted to NVS. Same write-only-on-change policy as Clock Face — Orion writes only when the user changes a slot. Orion reads Device Settings on (re)connect to recover the current slot tokens and populate its Quick Actions UI.
- **Clock Face** (`"c"`): persisted to NVS. Orion writes only when the user changes the setting in Orion's UI — not on every reconnect. Ori's NVS retains it across power cycles.
- **Time Format** (`"h"`): persisted to NVS. 0=24-hour (default), 1=12-hour. Governs every wall-clock display on Ori (status bar, both clock faces, meeting-list times, ANCS notification timestamps). Same write-only-on-change + read-back policy as Clock Face.
- **ANCS Filter** (`"f"`): persisted to NVS. Same write-only-on-change policy as Clock Face.

Orion can write any subset of fields in a single Device Settings write (e.g. presence-only on Teams state changes, slot update when the user changes a shortcut).

**Read on (re)connect:** Orion reads Device Settings once per connection to recover all six NVS-persisted fields: `"c"` (clock_face), `"h"` (time_format), `"f"` (ancs_filter), and `"1"`/`"2"`/`"3"` (shortcut slot tokens). This lets Orion's settings UI show the correct current values without having to cache what it last wrote across app restarts or BLE drops. Presence is not returned (ephemeral; Orion is the source of truth).

---

## 7. Disconnect, reconnect, and cache semantics

- Profile, photo, and Time Off persist to NVS on `SyncControl{op:"END"}` and are shown with a "SYNCED · X min ago" pill while offline. **Meeting List and local time are RAM-only** (not persisted, §6.0) — the pill therefore appears only on a *runtime* disconnect (meetings still in RAM), never after a power cycle (the list is empty → "No meetings today"). See `meeting-list.md`.
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

There is no wire-level protocol version negotiation and no compatibility gate — Ori and Orion are developed and released together, and the GATT layout/CBOR schema in this document is expected to stay stable. Unknown CBOR keys are always silently ignored (§4), so additive changes are non-breaking by construction; if a genuinely breaking change to this contract is ever needed, revisit this section then.

`fw_version` (semver, e.g. `"1.2.3"`) is read from the standard **Firmware Revision String** characteristic (§3.1) and drives exactly one thing: Orion polls `ori.app` for the latest released version and offers a user-initiated "Install update" in Settings when a newer build exists (`ota.md`). This check is purely optional and never blocks or gates the sync flow.

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
| `DeviceSettings.serial_number` | ≤ 32 chars (firmware `g_serial[32]`, `factory_info.cpp`) — read-only, provisioning.md |
| `DeviceSettings.manufacture_date` | ≤ 16 chars, ISO-8601 "YYYY-MM-DD" (firmware `g_mfg[16]`) — read-only, provisioning.md |
| `DeviceSettings.signal_bars` | uint 0–4 — read-only, live |
| `PhoneBondStatus.name` | ≤ 63 UTF-8 bytes (firmware `g_phone_name[64]` minus null terminator) |
| `PhoneBondStatus.missed_calls/unread_messages/total_notifications` | uint 0–255 (capped at `MAX_ANCS_NOTIFICATIONS` = 50 in practice) |
| `PhoneBondStatus.signal_bars` | uint 0–4 |
| Unpair Phone Command | exactly 4 bytes (0x55 0x4E 0x50 0x52) |
| `AncsNotification.icon_token` | ≤ 31 UTF-8 bytes — matches Ori's own on-device storage (`app_state.cpp`'s `AncsDetailEntry::token[32]`), not a wire-specific cap; longest known firmware token, `microsoft_authenticator`, is 23 |
| `AncsNotification.app` | ≤ 39 UTF-8 bytes — matches `AncsDetailEntry::display_name[40]` |
| `AncsNotification.title` | ≤ 192 UTF-8 bytes, truncated on a UTF-8 boundary past that — matches `AncsDetailEntry::title[193]` |
| `AncsNotification.body` | ≤ 512 UTF-8 bytes, truncated on a UTF-8 boundary past that — matches `AncsDetailEntry::body[513]`, which itself matches the 512-byte max Ori asks ANCS for (`ancs_client.cpp`'s `request_attributes()`, AttributeID 0x03). Nothing is lost between phone → Ori → Orion. Delivered chunked (§5's "AncsNotification chunking") since this alone exceeds one ATT notification |
| `AncsNotification.pos_label` / `neg_label` | ≤ 32 UTF-8 bytes each — matches `AncsDetailEntry::pos_label[33]`/`neg_label[33]` (real ANCS action labels are short standard strings, so this is rarely approached in practice) |
| `AncsNotification.category` | uint 0–12 |
| `AncsCallState.state` | uint 0–2 |
| `AncsCallState.icon_token` | ≤ 24 UTF-8 bytes — char 0011 is NOT chunked (calls carry no body, so the single-fragment squeeze that motivated raising `AncsNotification`'s caps doesn't apply here), so this stays independently capped at the original tighter size, same vocabulary as `AncsNotification.icon_token` |
| `AncsNotificationAction.action` | uint 0–1 |

`AncsNotification`'s caps are sized so a worst-case `"add"` (every optional field at max length) still fits one unchunked 238-byte fragment — see §13's "No chunking" note. If real-world CBOR encoding of max-length content ever measures over budget, tighten these caps further rather than adding chunking to a third notify-only characteristic.

---

## 11. Implementation owners

- **`esp32-connectivity`** — GATT server, bonding, chunk reassembly, NVS persistence + hashes, factory-reset routine, ANCS client, chars 10–18. No HOGP. Owns the filter-gated relay logic (§13) — the SAME filter evaluation used for the on-device status bar, not a second implementation.
- **`orion-sync`** — scanning + connection lifecycle, bonding storage, hash-manifest delta, chunked writes, background keep-alive, USB CDC OTA path (`ota.md`), media-mode OS bridge (§12), ANCS relay (§13). Reads char 15 (Phone Bond Status) on connect and subscribes to notifies; writes Unpair Phone magic bytes via char 8 (Device Command) on user request; writes char 14 (Device Settings) on reconnect (shortcuts + presence + weather), on every Teams presence change, on every weather-API poll that detects a change, and when the user changes clock face, time format, or ANCS filter. Subscribes to chars 16–17 (ANCS Notification, ANCS Call State) and maintains Orion's local notification mirror from them; writes char 18 (ANCS Notification Action) on Answer/Decline/End-call/Dismiss taps. Owns bringing the Orion window to the foreground on `AncsCallState{st:1}` (`orion-frontend` owns the in-app UI that follows).

Pre-release: no need to bump a version header per change — just keep this file in sync with the firmware/Orion implementations as the contract evolves.

---

## 12. Media-mode bridging — the Orion-mediated model

Ori uses chars 10–13 instead of HOGP. Orion bridges each `KeyboardCommand` notify to OS APIs and mirrors OS state changes back to Ori.

Orion is one Tauri v2 (Rust) codebase covering this contract on both platforms — see `memory.md`. The table below lists both OS columns since the underlying native APIs still differ per platform (called via Rust, behind `#[cfg(target_os = ...)]`); the macOS column is forward documentation to build against once that work starts, not yet implemented or verified.

### Command flow — Ori → Orion → OS

| User action | `KeyboardCommand` | Windows | macOS (planned) |
|---|---|---|---|
| Tap art | `{op:"play_pause"}` | `SendInput VK_MEDIA_PLAY_PAUSE` | `CGEventCreateMediaKeyEvent NX_KEYTYPE_PLAY` ¹ |
| Swipe right | `{op:"next"}` | `SendInput VK_MEDIA_NEXT_TRACK` | `NX_KEYTYPE_NEXT` ¹ |
| Swipe left | `{op:"prev"}` | `SendInput VK_MEDIA_PREV_TRACK` | `NX_KEYTYPE_PREVIOUS` ¹ |
| Vertical swipe release | `{op:"vol_set", arg:N}` | `IAudioEndpointVolume::SetMasterVolumeLevelScalar(N/100.0)` → write back `HostVolumeState` | `AudioObjectSetPropertyData kAudioHardwareServiceDeviceProperty_VirtualMainVolume` → write back |
| Tap shortcut slot N | `{op:"shortcut", arg:N}` | Orion runs configured action — see supported tokens below | same |

¹ Requires macOS Accessibility permission, granted on first launch; Controls-mode features stay inert until granted.

Supported shortcut actions (configured per slot in Orion settings): `vol-mute`, `mic-mute`, `screenshot`, `lock-screen`, `favorite-1`/`favorite-2`/`favorite-3` (three independently-configured user-defined custom actions, each with its own keyboard combo — `pc-app.md`), `calculator` (launch the OS calculator app), `copy`/`cut`/`paste`/`undo`/`redo`/`save` (standard edit shortcuts — Ctrl+C/X/V/Z/Y/S — replayed via the same key-injection path as a recorded Favorite combo, just with a fixed combo instead of a user-recorded one). No dedicated `mute` op — mute is the `vol-mute` shortcut.

### Shortcut icon assignment — Orion → Ori

Which icon shows in each of the three slots is configured in Orion's settings UI and delivered via the `"1"/"2"/"3"` fields of **Device Settings** (char `000E`, §3/§4) — written outside the BEGIN/END staging pipeline, applied immediately on Ori. **Persisted to NVS on Ori** (no manifest hash); Orion reads the current values back on every (re)connect and only writes when the user changes a slot (same write-only-on-change policy as Clock Face — see §6.4). Ori maps each token to a compiled-in icon asset (`shortcut_icons.h`); unknown tokens hide that slot's button entirely (`media-mode.md`) rather than failing the write. Adding a new icon *type* to the available set still requires a firmware update (`media-mode.md`) — Device Settings only carries which of the existing compiled-in icons each slot shows.

### State push flow — OS → Orion → Ori

- **Volume change:** `IAudioEndpointVolumeCallback` (Win) / `AudioObjectAddPropertyListener` (macOS, planned) → debounce ~100 ms → write `HostVolumeState`
- **Track change:** `GlobalSystemMediaTransportControlsSessionManager` (Win) / `MRMediaRemoteRegisterForNowPlayingNotifications` (macOS, planned — private `MediaRemote` framework; viable because Orion ships as a direct-download notarized app, never via the Mac App Store, see `memory.md` — but still re-verify it's usable when that work starts, since Apple can change private APIs between OS versions regardless of distribution channel) → write `MediaMetadata` + resize art to 484×216 JPEG (target 15–30 KB) → chunk-write `MediaAlbumArt`

### Swipe-vs-push race (vertical volume swipe)

While a vertical swipe is active (≥ 25 px threshold): Ori ignores incoming `HostVolumeState` writes. On lift: override drops, but Ori delays accepting the next push for ~800 ms (HUD linger). Orion debounces its OS volume push by ~100 ms.

### Cache + reconnect semantics

- `HostVolumeState` is read on (re)connect so the next swipe starts from the correct level.
- `MediaMetadata` and `MediaAlbumArt` are PSRAM-cached (not NVS); on reconnect Orion re-pushes the current track, or writes `MediaMetadata{title:"", artist:""}` if nothing is playing.

---

## 13. ANCS relay to Orion

Orion's iPhone Info modal drill-down (missed calls / messages / other notifications, tap-to-read, incoming-call banner) needs individual notification content and live call state — none of which chars 1–15 carry (`PhoneBondStatus`, char `000F`, is aggregate counts only). Chars 16–18 close that gap. `esp32-connectivity` owns the firmware side; `orion-sync` owns the BLE central + local mirror; `orion-frontend` owns the UI those events drive.

### Filter gates the relay, not just the display

Every notification Ori tracks is gated by the **same** `ancs_filter` level (Device Settings `"f"`, §3/§6.4) that already governs the on-device status bar — evaluated once, centrally, before either display path runs. **`Disabled` means Orion receives nothing from chars 16–17 at all** — no `AncsNotification{op:"add"}`, no `AncsCallState` transitions, not even for an incoming call. Orion's "bring the window to front on a ringing call" behavior therefore needs no filter check of its own on the Orion side: it simply never fires, because the notify that would trigger it never arrives. `CallOnly`/`Important`/`All` narrow or widen exactly which notifications qualify, identically to what's already documented for the status bar (`ancs_client.h`).

**Filter changes re-evaluate live state**, not just future events: when the user changes `"f"`, Ori reconciles Orion immediately by sending `AncsNotification{op:"clear"}` followed by `{op:"add"}` for every currently-queued notification that passes the new filter. This is deliberately a full clear-and-repopulate rather than a diff — firmware never has to track which uids it previously relayed (ordinary `queue_add`/`queue_remove` handling doesn't need that state either: relay `"add"` whenever a notification passes the filter at add/modify time, relay `"remove"` unconditionally on every `queue_remove` — a `"remove"` for a uid Orion never received is a harmless no-op on Orion's side). Orion's mirror always reflects "what the current filter allows," never a stale snapshot from connect time or from before the filter changed.

### Notification lifecycle

- `{op:"add"}` fires for a notification that (a) passes the current filter and (b) Ori hasn't already relayed — or that Ori HAS already relayed but iOS just sent a Modified event for (Orion replaces its stored copy in place, keyed by `"u"`; same semantics as firmware's own `app_state::set_ancs_detail`).
- `{op:"remove"}` fires whenever a previously-relayed notification leaves Ori's queue for **any** reason: user reads/dismisses it on Ori's own screen, the iPhone clears it, an ANCS Removed event arrives, or the queue evicts it (FIFO overflow past `MAX_ANCS_NOTIFICATIONS`). If the notification was never relayed (the filter was already excluding it), Ori does not send `"remove"` either — Orion never knew about it.
- **Orion must close a stale open detail.** If the notification currently shown in Orion's detail modal is removed, Orion closes the modal and returns to the drill-down list it was opened from (same path the UI's own action buttons already use to back out) — the content on screen no longer exists on the phone, so it can't stay open waiting for a tap that can no longer do anything.
- Stacking (same app + title grouped into one row, §-equivalent of firmware's `app_state::ancs_collect_same_title`) is a display-only concept on both sides — the wire only ever carries individual notifications; both UIs group them locally.

### Call takeover

`AncsCallState{st:1}` (ringing) is Orion's cue to raise itself the same way clicking its taskbar entry would (Tauri: show the window, set focus, un-minimize if minimized) and present the incoming-call view — the point is the user notices a call is happening even if Orion was in the background. `{st:2}` (active — e.g. Orion reconnects mid-call) resumes the in-call view with its duration timer seeded from `"e"` instead of restarting at zero. `{st:0}` closes whatever call view is open (declined/ended from the phone itself, or the iPhone link dropping — `ancs_client`'s existing `close_all()` on iPhone disconnect covers the latter).

### Actions

Orion writes `AncsNotificationAction` only in direct response to a user tap (Answer / Decline / End call / Dismiss / Read all — one write per UID for a "Read all" on a stacked group, mirroring firmware's own `on_read()` loop in `modal_ancs_notification.cpp`). Ori performs the identical action an on-device tap would trigger — there is no separate "remote action" code path in `ancs_client`, just a second caller — including for a Negative (`"a":1`) the same **dismiss-vs-drop** choice the on-device swipe makes: `dismiss_notification()` (send the ANCS Negative) only when the notification has a negative action, else `drop_notification()` (local queue removal, no ANCS write). The **removal relay back to Orion is emitted from `queue_remove()` itself** — a single choke-point that fires `AncsNotification{op:"remove"}` (normal) or `AncsCallState{st:0}` (call) while the notification's category is still live — so it lands correctly no matter which path removed it (phone-side Removed event, local dismiss, or this Orion-relayed action). It must NOT be emitted from the phone's own Removed-event handler, which runs after a local dismiss has already dropped the category and would misfire. Orion does **not** update its own UI optimistically on write: it waits for the `AncsNotification{op:"remove"}` / `AncsCallState` transition the resulting ANCS event produces, same as every other state change in this protocol (§6 never assumes a write succeeded before the corresponding notify confirms it).

### Chunked, matching Ori's own storage

`AncsNotification`'s field caps (§10) match Ori's own on-device storage exactly (`app_state.cpp`'s `AncsDetailEntry`) rather than being squeezed to fit one ATT notification — Orion never shows less body text than Ori itself does for the same notification, and `body` specifically matches the 512-byte max Ori itself requests from ANCS (`ancs_client.cpp`'s `request_attributes()`) — nothing is lost at any of the three hops (phone → Ori → Orion). Since a maxed-out `body` (512 bytes) alone exceeds a single fragment, `"add"` is sent chunked (§5's "AncsNotification chunking" — reuses the same frame format as the write-direction chunking, in reverse, with no NACK/retry or windowed flow control since neither is needed at this scale). `"remove"`/`"clear"` are sent through the same always-framed path for consistency (as a single `total_frags:1` frame) even though they never need more than one — this way Orion's reassembler never has to guess whether a given notify is a bare CBOR payload or a chunk frame. Full content is still always available by checking the phone regardless — this is a generous cap, not an unlimited one, since the underlying notification could still be longer than the 512 bytes Ori explicitly asks ANCS for.

### Icon tokens

`"k"` reuses firmware's existing per-bundle icon token vocabulary (`ancs_icons.h`, `firmware.md`'s 49-token list) — Orion needs matching icon assets keyed by the same tokens (or a category-based fallback glyph, mirroring `ancs_icons::category_image()`) to stay visually consistent with what's shown on the device itself. Adding a new brand icon is a firmware change on Ori's side (`firmware.md`) plus an asset addition on Orion's, same two-sided update the device's own icon set already requires.
