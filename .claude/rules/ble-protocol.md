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

LE Secure Connections with Passkey Entry (6-digit numeric, MITM-protected) is mandatory. After first pairing the device is bonded; subsequent reconnects are silent. Passkey confirmation itself is handled by the OS's own Bluetooth pairing UI on both platforms, not a custom Orion screen — see §6.1 and `memory.md`'s pairing-UX decision.

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

**One service, fifteen characteristics** — plus a separate BLE SIG standard service for firmware version (§3.1).

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
| 10 | **Keyboard Command** | `000A` | Notify | Ori → Orion (notify) | Yes |
| 11 | **Host Volume State** | `000B` | Read, Write (response) | Orion → Ori (+ Orion reads) | Yes |
| 12 | **Media Metadata** | `000C` | Write, Notify | Orion → Ori | Yes |
| 13 | **Media Album Art** | `000D` | Write (no response) | Orion → Ori (chunked) | Yes |
| 14 | **Device Settings** | `000E` | Read, Write (response) | Orion → Ori (+ Orion reads) | Yes |
| 15 | **Phone Bond Status** | `000F` | Read, Notify | Ori → Orion (notify) | Yes |

Reads/writes on encrypted characteristics over an unencrypted link return `INSUFFICIENT_AUTHENTICATION`.

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
// Read (Orion → Ori): returns all NVS-persisted fields —
//   {"c": <clock_face>, "h": <time_format>, "f": <ancs_filter>, "1": <slot1>, "2": <slot2>, "3": <slot3>}.
// Presence and weather are not returned (both ephemeral; Orion is source of truth).
// Orion reads on (re)connect to restore its settings UI without having to cache what it last wrote.

PhoneBondStatus = {            // Ori → Orion, notify + readable (CBOR)
  "b": bool,     // bonded.    true = iPhone NVS slot is occupied (a bond exists).
  "c": bool,     // connected. true = BLE link to iPhone is currently up.
  "n": text      // name.      iPhone's GAP Device Name (e.g. "Xander's iPhone"),
                 //            or "" when not connected / read failed. ≤ 63 UTF-8 bytes.
}
// Ori notifies Orion on every iPhone state change: bond formed, connected
// (reconnect or first bond), disconnected (plain disconnect or after wipe).
// Orion reads on (re)connect to recover initial state without waiting for a
// notify. Value is always kept current; the stored characteristic value equals
// the last-notified state.

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
```

Any other value → `NACK_BAD_MAGIC` via SyncControl notify.

---

## 5. Chunking protocol

Used by Profile Photo, Meeting List, Time Off Entry, and Media Album Art.

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
| `PhoneBondStatus.name` | ≤ 63 UTF-8 bytes (firmware `g_phone_name[64]` minus null terminator) |
| Unpair Phone Command | exactly 4 bytes (0x55 0x4E 0x50 0x52) |

---

## 11. Implementation owners

- **`esp32-connectivity`** — GATT server, bonding, chunk reassembly, NVS persistence + hashes, factory-reset routine, ANCS client, chars 10–15. No HOGP.
- **`orion-sync`** — scanning + connection lifecycle, bonding storage, hash-manifest delta, chunked writes, background keep-alive, USB CDC OTA path (`ota.md`), media-mode OS bridge (§12). Reads char 15 (Phone Bond Status) on connect and subscribes to notifies; writes Unpair Phone magic bytes via char 8 (Device Command) on user request; writes char 14 (Device Settings) on reconnect (shortcuts + presence + weather), on every Teams presence change, on every weather-API poll that detects a change, and when the user changes clock face, time format, or ANCS filter.

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

Supported shortcut actions (configured per slot in Orion settings): `vol-mute`, `mic-mute`, `screenshot`, `lock-screen`, `favorite` (user-defined custom action), `calculator` (launch the OS calculator app). No dedicated `mute` op — mute is the `vol-mute` shortcut.

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
