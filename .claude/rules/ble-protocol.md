# Ori — BLE GATT Protocol Specification

**Protocol version:** 1.0 — pre-release; this contract isn't finalized, no version-history tracking needed yet.
**Status:** Authoritative — `esp32-connectivity` (Ori firmware) and `orion-sync` (Orion app) must conform.

This document defines the single BLE GATT contract between Ori and the Orion PC app.

**Out of scope for BLE:** ANCS phone link (`connectivity.md`); firmware updates run over USB CDC (`ota.md`).

---

## 1. Roles

| Side | Role |
|---|---|
| **Ori (Arduino on ESP32-S3)** | GATT server + Advertiser. Hosts every characteristic below. |
| **Orion (Flutter, Windows/macOS)** | GATT client. The only side that can issue Read/Write requests. |

LE Secure Connections with Passkey Entry (6-digit numeric, MITM-protected) is mandatory. After first pairing the device is bonded; subsequent reconnects are silent.

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

**One service, sixteen characteristics.**

```
Ori Sync Service:  6F726900-0000-4F72-9F00-000000000000
```

Each characteristic UUID replaces bytes 4–5 of the base with the offset below.

| # | Name | UUID offset | Properties | Direction | Encrypted? |
|---|---|---|---|---|---|
| 1 | Protocol Version | `0001` | Read | Orion reads | No |
| 2 | Device Status | `0002` | Read, Notify | Ori → Orion (notify) | No |
| 3 | Time Sync | `0003` | Write (response) | Orion → Ori | Yes |
| 4 | Profile Info | `0004` | Write (response) | Orion → Ori | Yes |
| 5 | Profile Photo | `0005` | Write, **Write NR** | Orion → Ori (chunked) | Yes |
| 6 | Meeting List | `0006` | Write, **Write NR** | Orion → Ori (chunked) | Yes |
| 7 | PTO Entry | `0007` | Write, **Write NR** | Orion → Ori (chunked) | Yes |
| 8 | Sync Control | `0008` | Write, Notify | Orion ↔ Ori | Yes |
| 9 | Factory Reset Command | `0009` | Write (response) | Orion → Ori | Yes |
| 10 | Sync Manifest | `000A` | Write, Notify | Orion ↔ Ori | Yes |
| 11 | **Keyboard Command** | `000B` | Notify | Ori → Orion (notify) | Yes |
| 12 | **Host Volume State** | `000C` | Read, Write (response) | Orion → Ori (+ Orion reads) | Yes |
| 13 | **Media Metadata** | `000D` | Write, Notify | Orion → Ori | Yes |
| 14 | **Media Album Art** | `000E` | Write (no response) | Orion → Ori (chunked) | Yes |
| 15 | **Presence Status** | `000F` | Write (response) | Orion → Ori | Yes |
| 16 | **Shortcut Config** | `0010` | Write (response) | Orion → Ori | Yes |

Reads/writes on encrypted characteristics over an unencrypted link return `INSUFFICIENT_AUTHENTICATION`.

---

## 4. Payload encoding — CBOR

All structured payloads use CBOR (RFC 8949). Libraries: Arduino — `ArduinoCBOR`; Dart — `cbor` pub package. Unknown keys are silently ignored (forward-compatible).

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
  "z": text,           // tz. IANA timezone, e.g. "Europe/Lisbon"
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

PtoEntry = {
  "s": uint,           // start
  "e": uint,           // end
  "d": text,           // destination, e.g. "Lisbon, Portugal"
  "m": bytes           // image. JPEG (528×396 target); may be empty. Orion resizes before sending.
}

ShortcutConfig = {                // RAM-only, no hash — always sent, like TimeSync (§6.0)
  "1": text,           // slot1. icon token, ≤ 19 chars — "vol-mute", "mic-mute", "screenshot",
  "2": text,           // slot2. "lock-screen", or "favorite" (media-mode.md). Unknown tokens
  "3": text            // slot3. fall back to a neutral icon rather than failing the sync.
}

SyncControl = {
  "o": "BEGIN" | "END" | "ACK" | "NACK",  // op
  "s": uint,           // seq
  "r": text,           // reason. optional, populated for NACK
  "t": uint            // total. optional, BEGIN only — total application-payload bytes Orion
                        // will write across Time Sync, Shortcut Config, Profile Info, Profile
                        // Photo, Meeting List, PTO Entry for this sync session (Time Sync and
                        // Shortcut Config unconditionally; the rest only the items actually
                        // being sent — e.g. just the `needed` subset on a reconnect delta
                        // sync). Absent or 0 = progress is indeterminate.
}

SyncManifest_Write = {           // Orion → Ori — no shortcut entry; RAM-only, always resent
  "p": bytes(32),      // profile_sha
  "h": bytes(32),      // photo_sha
  "m": bytes(32),      // meetings_sha
  "t": bytes(32)       // pto_sha
}

SyncManifest_Notify = {          // Ori → Orion
  "n": [text, ...]               // needed. subset of {"profile","photo","meetings","pto"}
}

ProtocolVersion = {
  "j": uint,           // proto_major
  "n": uint,           // proto_minor
  "f": text            // fw_version. semver, e.g. "1.2.3"
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
  "c": bool            // can_seek. optional; absent = false. Ori hides scrubber when false.
}

// Media Album Art — raw JPEG bytes (not CBOR), via §5 chunking. Orion resizes to 484×216.

// Presence Status — single byte (NOT CBOR):
//   0x00  AVAILABLE   — Teams "Available"
//   0x01  BUSY        — Teams "Busy", "Do Not Disturb", "In a call", "In a meeting", "Presenting"
//   0x02  AWAY        — Teams "Be Right Back", "Appear Away"
//   0x03  OFFLINE     — Teams "Appear Offline" (or unknown / null)
// Any other value → NACK_CBOR_DECODE.
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

### Factory Reset Command (4-byte payload)

```
0xFA 0xC7 0x5E 0x5E    // "FAC75E5E"
```

Any other value → `NACK_BAD_MAGIC`.

---

## 5. Chunking protocol

Used by Profile Photo, Meeting List, PTO Entry, and Media Album Art.

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

Profile Photo, Meeting List, and PTO Entry advertise **both** `Write` and
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
  card, meeting list, PTO, hashes) changes. If the link drops mid-session, Ori discards
  the staged data and keeps serving the previously cached data — a partial transfer
  can never corrupt or partially overwrite NVS.
- At `END`, Ori parses each staged item, computes its SHA-256, and updates the live UI
  — in one batch. **Profile, Profile Photo, and PTO Entry persist to NVS.**
- **Time Sync, Meeting List, and Shortcut Config are all RAM-only on Ori** — staged
  and applied (to RAM + live UI) at `END` like the rest, but NOT written to NVS.
  Meeting List and Shortcut Config also have no manifest hash; Orion just resends
  Shortcut Config unconditionally on every sync, the same way it always sends Time
  Sync regardless of whether anything changed. A power cycle drops all three back to
  their defaults; the next sync repopulates them. **The wire is unchanged** for
  Meeting List — Orion sends it identically; this is purely Ori's storage choice (no
  `proto` bump). Why Meeting List specifically: with no battery-backed RTC the local
  clock isn't restored on a cold boot, so the meeting time logic can't run offline
  anyway — see `meeting-list.md`. Why Time Sync: the epoch is inherently RAM-only
  (`settimeofday()`, nothing to persist), and the `tz` string rides along — it used to
  be persisted separately (`nvs_sync::save_tz()`), but nothing ever read it back at
  boot, so it was a pure write with no benefit; now it's RAM-only too. Why Shortcut
  Config: it's small enough, and changes rarely enough, that hash-checking it costs
  more than just always sending it.
- **Display blackout during the flash write.** Right before committing, Ori blanks the
  whole framebuffer (`lcd_panel::blackout()`) so LCD_CAM DMA doesn't show glitches
  while NVS flash writes briefly disable the CPU cache (`hardware.md`). This only runs
  when the commit actually touches NVS — Profile, Profile Photo, or PTO Entry. A
  commit containing only Time Sync, Meeting List, and/or Shortcut Config (all
  RAM-only) skips it entirely, and the UI updates with no screen flicker.
- **Progress.** If `BEGIN` includes `total` (> 0), Ori tracks cumulative bytes received
  across Time Sync, Shortcut Config, Profile Info, Profile Photo, Meeting List, and
  PTO Entry writes — for chunked characteristics, each fragment's `payload_len` (not
  the 6-byte frame header) counts towards the total — and derives
  `pct = received * 100 / total` (capped at 99 until `END` finishes the commit, then
  100). This drives the **Step 2/3 "Orioning" progress ring** during first-time setup
  (`setup-flow.md`) AND the runtime reconnect **"Refreshing your day"** overlay
  (`state-machine.md`) — both are driven by the same `OrioningProgress` event;
  whichever screen isn't actually live no-ops on it harmlessly. The runtime
  overlay itself is only shown when `total` exceeds `RECONNECT_OVERLAY_MIN_BYTES`
  (200 B) — see `state-machine.md`'s Reconnect-Syncing Overlay section.
- If `total` is absent or 0, Ori still stages-then-commits as above, but the orioning
  ring is not driven by byte progress (legacy/indeterminate).

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
Orion computes total = byte-length of (Time Sync + Shortcut Config + Profile Info
                                        + Profile Photo + Meeting List + PTO Entry) payloads
Orion writes SyncControl{op:"BEGIN", seq:1, total:total}
  → HANDSHAKE: a valid SyncControl{BEGIN} on the encrypted link is Ori's proof
    that the bonded peer is the Orion app (not a phone or other PC someone paired
    off the passkey screen). On this BEGIN, Ori commits the provisional address
    to the orion_addr slot. A peer that bonds but never sends a valid BEGIN
    within ~5 s is disconnected and its LTK bond deleted — never saved as Orion.
Orion writes Time Sync
Orion writes Shortcut Config
Orion writes Profile Info
Orion writes Profile Photo (chunked)
Orion writes Meeting List (chunked)
Orion writes PTO Entry (chunked)
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

Orion writes Time Sync (always — ~20 bytes, clock may drift)
Orion writes Sync Manifest: { profile_sha, photo_sha, meetings_sha, pto_sha }
Ori compares against NVS/RAM hashes → notifies Sync Manifest: { needed: [...] }

Orion computes total = byte-length of (Time Sync + Shortcut Config + only the
                                        `needed` items)
Orion writes SyncControl{op:"BEGIN", seq:N, total:total}
Orion writes Shortcut Config (always — no hash, RAM-only on Ori, like Time Sync)
Orion writes only requested items (profile → photo → meetings → pto)
Orion writes SyncControl{op:"END", seq:N}

Ori commits all staged items to NVS + hashes atomically (§6.0).
Ori notifies Device Status = RUNTIME_READY → overlay dismissed.
```

**Hash content:** SHA-256 of canonical deterministic CBOR (sorted keys, smallest-encoding ints). If hashes drift for any reason, next reconnect re-pushes automatically.

### 6.3 Periodic refresh (while RUNTIME_READY)

| Trigger | Cadence | Effect |
|---|---|---|
| Time Sync | Every 10 min | Write Time Sync |
| Meeting List | Calendar event or every 15 min | Hash-check via Manifest, push if needed |
| PTO Entry | Calendar event | Hash-check, push if needed |
| Profile Info / Photo | User edit in Orion | Hash-check, push if needed |
| Shortcut Config | Every sync (no hash) | Always sent, like Time Sync |
| Presence Status | Teams change or ~60 s poll | Write Presence Status (only when value changes) |

Periodic refreshes set `RUNTIME_SYNCING` briefly but do **not** trigger the reconnecting overlay.

### 6.4 Presence push (no manifest, no hash)

Presence Status is ephemeral — not in the manifest flow, not persisted to NVS,
and **Write-only** (no `Read`): Orion is the sole source of
truth and always knows the live value, so there's nothing for Ori to recover
that Orion doesn't already have — a Read would only ever echo back what Orion
itself last wrote, and the write is already `Write (response)` so delivery is
already confirmed without one.

- On (re)connect: Orion **writes** a fresh value immediately — Ori has no way
  to recover presence on its own after a disconnect.
- Between connects: Orion **writes** again on every Teams state change.
- Before the first write: display `Offline`.
- On BLE link drop: immediately render `Offline` (stale presence would lie about reality).

---

## 7. Disconnect, reconnect, and cache semantics

- Profile, photo, and PTO persist to NVS on `SyncControl{op:"END"}` and are shown with a "SYNCED · X min ago" pill while offline. **Meeting List and local time are RAM-only** (not persisted, §6.0) — the pill therefore appears only on a *runtime* disconnect (meetings still in RAM), never after a power cycle (the list is empty → "No meetings today"). See `meeting-list.md`.
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
| `NACK_BAD_MAGIC` | Factory Reset Command wrong magic |

Link-layer encryption failure (`BLE_HS_ENC_FAIL`) signals a stale bond — see §7.1. Surface it cleanly; do not retry-loop.

---

## 9. Versioning

- Protocol Version characteristic: `{ proto_major, proto_minor, fw_version }`. Pre-release — this contract isn't finalized, so no changelog is kept here; just edit the relevant section in place when something changes.
- Once the protocol is finalized/released, switch to: **Major bump** = breaking (Orion refuses sync, shows "Update Ori firmware"); **Minor bump** = additive (unknown CBOR keys silently ignored).
- `fw_version` (semver) used by Orion to detect available updates; update runs over USB CDC (`ota.md`).

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
| `PtoEntry.destination` | ≤ 128 UTF-8 bytes |
| PTO image (JPEG, 528×396) | hard cap 512 KB — Orion resizes to 528×396 before sending |
| `MediaMetadata.title` | ≤ 192 UTF-8 bytes |
| `MediaMetadata.artist` | ≤ 96 UTF-8 bytes |
| Media Album Art (JPEG, 484×216) | target 15–30 KB; hard cap 64 KB |
| Presence Status | exactly 1 byte (0x00–0x03) |
| `ShortcutConfig.slot1/2/3` | ≤ 19 chars each (firmware buffer) — icon token, e.g. "vol-mute" |

---

## 11. Implementation owners

- **`esp32-connectivity`** — GATT server, bonding, chunk reassembly, NVS persistence + hashes, factory-reset routine, ANCS client, chars 11–16. No HOGP.
- **`orion-sync`** — scanning + connection lifecycle, bonding storage, hash-manifest delta, chunked writes, background keep-alive, USB CDC OTA path (`ota.md`), media-mode OS bridge (§12).

Pre-release: no need to bump a version header per change — just keep this file in sync with the firmware/Orion implementations as the contract evolves.

---

## 12. Media-mode bridging — the Orion-mediated model

Ori uses chars 11–14 instead of HOGP. Orion bridges each `KeyboardCommand` notify to OS APIs and mirrors OS state changes back to Ori.

### Command flow — Ori → Orion → OS

| User action | `KeyboardCommand` | Windows | macOS |
|---|---|---|---|
| Tap art | `{op:"play_pause"}` | `SendInput VK_MEDIA_PLAY_PAUSE` | `CGEventCreateMediaKeyEvent NX_KEYTYPE_PLAY` ¹ |
| Swipe right | `{op:"next"}` | `SendInput VK_MEDIA_NEXT_TRACK` | `NX_KEYTYPE_NEXT` |
| Swipe left | `{op:"prev"}` | `SendInput VK_MEDIA_PREV_TRACK` | `NX_KEYTYPE_PREVIOUS` |
| Vertical swipe release | `{op:"vol_set", arg:N}` | `IAudioEndpointVolume::SetMasterVolumeLevelScalar(N/100.0)` → write back `HostVolumeState` | `AudioObjectSetPropertyData kAudioHardwareServiceDeviceProperty_VirtualMainVolume` → write back |
| Tap shortcut slot N | `{op:"shortcut", arg:N}` | Orion runs configured action — see supported tokens below | same |

¹ Requires macOS Accessibility permission, granted on first launch.

Supported shortcut actions (configured per slot in Orion settings): `vol-mute`, `mic-mute`, `screenshot`, `lock-screen`, `favorite` (user-defined custom action). No dedicated `mute` op — mute is the `vol-mute` shortcut.

### Shortcut icon assignment — Orion → Ori

Which icon shows in each of the three slots is configured in Orion's settings UI and delivered over **Shortcut Config** (char `0010`, §3/§4) — still staged like Profile Info (applied at `SyncControl{END}`), but RAM-only on Ori and sent unconditionally on every sync, like Time Sync — no NVS, no hash, no manifest entry. It's small and changes rarely enough that hash-checking it costs more than just always sending it, and skipping NVS means a shortcuts-only sync never needs the display blackout (§6.0). Ori maps each token to a compiled-in icon asset (`shortcut_icons.h`); unknown tokens hide that slot's button entirely (`media-mode.md`) rather than failing the sync. Adding a new icon *type* to the available set still requires a firmware update (`media-mode.md`) — this characteristic only carries which of the existing compiled-in icons each slot shows.

### State push flow — OS → Orion → Ori

- **Volume change:** `IAudioEndpointVolumeCallback` (Win) / `AudioObjectAddPropertyListener` (macOS) → debounce ~100 ms → write `HostVolumeState`
- **Track change:** `GlobalSystemMediaTransportControlsSessionManager` (Win) / `MRMediaRemoteRegisterForNowPlayingNotifications` (macOS) → write `MediaMetadata` + resize art to 484×216 JPEG (target 15–30 KB) → chunk-write `MediaAlbumArt`

### Swipe-vs-push race (vertical volume swipe)

While a vertical swipe is active (≥ 25 px threshold): Ori ignores incoming `HostVolumeState` writes. On lift: override drops, but Ori delays accepting the next push for ~800 ms (HUD linger). Orion debounces its OS volume push by ~100 ms.

### Cache + reconnect semantics

- `HostVolumeState` is read on (re)connect so the next swipe starts from the correct level.
- `MediaMetadata` and `MediaAlbumArt` are PSRAM-cached (not NVS); on reconnect Orion re-pushes the current track, or writes `MediaMetadata{title:"", artist:""}` if nothing is playing.
