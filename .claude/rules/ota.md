# Ori — Firmware Update

Firmware updates run over **BLE**, on the same bonded Orion link that carries everything else. There is no USB firmware-update path and no USB-MSC — Ori must never appear as a removable drive, and the enclosure exposes no reachable data port. Bricked-unit recovery uses the internal UART port — service path only.

> **Changed 2026-08-16.** Updates used to run over USB CDC on the USB-C power cable. That assumed someone could reach a data-capable port on a deployed unit, which is no longer true: the enclosure exposes power only. A field unit has to be updatable over the air or not at all, so the transport moved to BLE and the USB CDC receiver was deleted outright rather than kept as a fallback nobody can reach. Everything downstream of the transport — PSRAM staging, SHA-256, the embedded-version check, the single flash commit, the on-screen flow, rollback — is unchanged.

> **On the word "OTA":** the code says "OTA" (file names, the `OTA_UPDATING` state, the `ota_0`/`ota_1` partitions) because that's the ESP-IDF term for a *slot-swap firmware update*. It is now also literally over-the-air. User-facing copy still says "firmware update," never "OTA."

| Property | Value |
|---|---|
| Initiator | Orion (Settings → "Install update") |
| Transport | BLE — Ori Sync Service chars `0014`/`0015` (`ble-protocol.md` §14) |
| Authority | The Orion bond. MITM-encrypted, and Ori additionally checks the writer is the bonded Orion peer — the iPhone bond can never reach these characteristics |
| Time for a ~2.3 MB image | **~95 s measured** — see "Measured throughput" below before quoting a faster number |
| Driver requirement | None |
| Physical requirement | Ori powered and in BLE range of the PC |

## Measured throughput

First real-hardware numbers, 2026-08-17, Ori ↔ Windows over `tools/mock_orion_ble_ota.py`. Recorded because several of them contradict what was assumed when the transport was designed.

| Quantity | Measured |
|---|---|
| End-to-end, 2,274,880 B image | **94.6 s** (~23–24 KB/s) |
| Connection interval | 15 ms (Ori requests 7.5–15; Windows grants the 15 ms end) |
| PHY | **1M** — `phy tx=1 rx=1` mid-transfer, *not* 2M |
| ATT MTU | 247, as intended |
| Write-No-Response | ~1 ms |
| Write-with-response | **~220 ms** |

Three things this rules out, so they don't get re-investigated:

- **Sender parallelism is not the lever.** Keeping 1, 4, 8, or 16 Write-No-Response frames outstanding gives 23.9 / 23.3 / 23.7 / 22.7 KB/s — no difference, and no `RESUME` churn (ordering holds).
- **The checkpoints are not additive overhead.** Removing all 197 of them from a half-image transfer left throughput unchanged. Their ~220 ms cost is the sender blocking while the radio drains a backlog it is already ahead of — not time added to the transfer.
- **The interval is not the problem.** 15 ms is granted and applied.

**What is still unexplained:** at 1M PHY, 15 ms, and 251-byte LL PDUs, one packet plus its ack is ~2.5 ms, so a connection event has room for roughly 6 packets ≈ 97 KB/s. The link delivers about 1.5. The sender is demonstrably ahead (it blocks against its own window while Windows has the data queued), so the constraint is downstream of the sender.

The untested hypothesis is Ori's controller→host ACL credit pool: `CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` defaults to **12** in NimBLE-Arduino, and the controller may not hand the host more packets than it has buffers for — a hard per-event ceiling regardless of airtime. Raising it (with `CONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT` alongside, since msys blocks back L2CAP reassembly) is the next thing to try. It was built and flashed once but **never cleanly measured**, so it is neither confirmed nor ruled out.

Also worth knowing before chasing this: `NimBLEDevice::setDefaultPhy()` in `ble_manager::init()` only states a preference for *new* connections. It does not renegotiate an established link and does not make the central ask, which is why the link sits at 1M despite that call. Moving an existing link to 2M needs an explicit per-connection `ble_gap_set_prefered_le_phy()`, which Ori deliberately does not issue today.

## Firmware implementation

- Arduino **`Update`** library: `Update.begin()`, `Update.write(buf, len)`, `Update.end(true)`.
- Two 3 MB OTA slots (`ota_0`, `ota_1`) + OTA-data partition — see `firmware/partitions.csv`.
- Pending-verify window: **60 seconds** after first boot. If new firmware fails (panic, watchdog, no BLE), the bootloader rolls back automatically on next boot.
- Receiver: `firmware/src/ota_receiver.cpp`. The two write callbacks run on the **NimBLE host task**; everything touching LVGL, NVS, or the heap is deferred to `ota_receiver::poll()` on the main task via the existing BLE event queue (`FwUpdateBegin`/`FwUpdateEnd`/`FwUpdateAbort`).

## Wire protocol

Two characteristics on the Ori Sync Service. Full schema in `ble-protocol.md` §14; the shape:

**Control (`0014`, Write + Notify, CBOR, single-char keys)**

```
Orion → Ori   { "o": "BEGIN", "v": <version>, "n": <total_size>, "h": <sha256 32 B> }
              { "o": "END" }
              { "o": "ABORT" }

Ori → Orion   { "o": "READY" }
              { "o": "REJECT",    "r": <reason> }
              { "o": "PROGRESS",  "b": <bytes_received> }
              { "o": "RESUME",    "b": <offset Ori wants next> }
              { "o": "VALIDATED", "v": <version read out of the binary> }
              { "o": "FAILED",    "r": <reason> }
```

**Data (`0015`, Write-No-Response + Write, raw)**

```
Offset  Size  Field
0       4     offset (uint32 LE) — absolute position of this payload in the image
4       N     image bytes
```

### Why an offset, not the §5 chunk header

Every other chunked payload on this service uses §5's `seq_num`/`total_frags`/`payload_len` frame. Firmware update deliberately does not:

- **A lost fragment costs one rewind, not the whole image.** §5's recovery is "NACK, sender restarts from seq 0" — acceptable for a 30 KB album art, absurd for 1.5 MB over a link that will drop the occasional packet. With an absolute offset, Ori answers `RESUME { b }` and the sender seeks there.
- **No fragment-count ceiling.** `total_frags` is a uint16. At the §5 fallback MTU of 23 (14-byte payloads) a 1.5 MB image is ~107,000 fragments — an overflow. An offset has no such limit.
- **Ori's hash is computed once at END over the staged buffer, not streamed.** That is what makes the rewind free: a streaming SHA-256 context cannot be rewound, so every resync would have to re-hash anyway.

Ori accepts a frame only when `offset == bytes_received`. Anything else — a gap or a retransmit — is dropped and answered with `RESUME`, rate-limited to one per 200 ms so the sender's in-flight window doesn't produce one notify per stale frame.

## Wire flow

```
Orion checks orinari.net for latest version vs fw_version read from the standard
Firmware Revision String characteristic (Device Information Service, BLE).
If newer exists:
   Orion UI: Settings → "Update available · 1.2.3" → tap "Install update"

Orion requires an established, encrypted Orion bond (it already has one — it is
the sync link). If Ori is not connected: prompt "Ori isn't connected. Make sure
it's powered and in range, then try again."

Orion writes Control BEGIN { v: fw_version, n: total_size, h: sha256 }

Ori validates: required fields present; size <= inactive slot capacity; PSRAM
staging buffer allocates; no transfer already in progress.
   On reject: notify REJECT { r: reason }; return to normal runtime.
   On accept: allocate the PSRAM staging buffer;
              NACK every other BLE data characteristic;
              enter transfer quiet mode (below);
              request a 7.5-15 ms connection interval;
              switch screen to OTA-Updating;
              notify READY.

Orion streams Data frames. Firmware copies each into the PSRAM buffer.
Every ~1% (capped at 16 KB), Ori notifies PROGRESS { b: bytes_received }.
On a gap, Ori notifies RESUME { b } and Orion rewinds. After the last frame,
Orion writes Control END.

Ori computes SHA-256 over the staged image and checks the embedded version.
   On pass:   notify VALIDATED { v }; show "Installing"; linger 3.5 s;
              tear down BLE; halt LCD; write flash; ESP.restart().
   On fail:   notify FAILED { r }; discard the image; resume runtime.

After reboot:
   Bootloader runs new firmware in pending-verify state.
   Firmware brings up BLE and advertises to bonded peers.
   60 s healthy → mark partition valid.
   60 s unhealthy (panic / watchdog) → bootloader rolls back on next boot.

Orion reconnects over BLE, reads the Firmware Revision String characteristic,
confirms new fw_version.
```

### VALIDATED comes before the commit, not after

Over USB, `VALIDATED` was sent *after* `Update.end(true)` succeeded. Over BLE it cannot be: the commit calls `ble_manager::quiesce_for_commit()` (`NimBLEDevice::deinit`) so nothing BLE-side can execute non-IRAM code or trigger an NVS write while `Update.write()` has the MSPI cache disabled. The stack is gone before the first byte reaches flash.

So `VALIDATED` now means **"image received, verified, and about to be installed"** — sent at the end of the verification pass, with the 3.5 s Installing linger giving it ample time to reach Orion before teardown. A flash failure after that point surfaces as **"Ori rebooted and is still running the old version"**, which Orion checks after every update anyway. There is no `flash_error` frame on the wire any more; the reason code still exists for the on-screen message.

## On-device UX during update

All firmware-update screens are full-screen takeovers (status bar + profile card + left panel hidden) built in `firmware/src/screens/screen_ota_updating.cpp`. The flow is **automatic**: once the image is downloaded and verified, the device advances straight to the Installing frame and commits after a short linger — **no user confirmation step**.

### Screen sequence

| # | Screen | Builder | Content | Notes |
|---|---|---|---|---|
| 1 | **Updating firmware** | `create()` | Title + live progress ring (220 px, % in centre) + "Keep Ori near your PC" | Ring driven by `set_progress()` from `ota_receiver::poll()`, once per integer percent of bytes received; image streams into PSRAM (LCD stays live). State `AwaitingData`. |
| 2 | **Installing** | `set_installing(linger_ms)` | "Installing firmware" title + centred instruction + bottom countdown bar | Reached automatically at END after the hash + version check pass (state `Installing`). Reuses screen 1's objects (see layout below). Shown for `linger_ms` (= `COMMIT_LINGER_MS`, 3.5 s), then the LCD halts for the flash burst. |
| — | *(commit)* | — | Screen **dark** | LCD halted (PSRAM-DMA vs flash MSPI contention), BLE torn down, image written to flash, then reboot. No frame can be shown between flash and reboot. |
| 3 | **Firmware updated** | `create_updated_ack(version, on_close)` | "Firmware updated" title + "Ori is now running version X" + animated check + **Close** tertiary button | Post-reboot acknowledgement. **Persisted in NVS** (`ota_ack` key) — reappears on every boot until the user taps Close. Serves as the completion confirmation (there is no separate "Update complete / Restart" screen — the install→reboot is atomic). |
| 4 | **Update failed** | `create_error(message, on_close)` | "Update failed" title + warning glyph + plain-language reason + **Close** tertiary button | Shown for in-flight failures and user-relevant BEGIN rejects. Wire still carries the terse code; `friendly_reason()` maps it to the on-screen sentence. |

There is **no "Firmware Install / Update now" gate** on the device — the download flows straight into the install. (The user's confirmation happened in Orion.)

### Shared layout conventions

- **Title** uses `font_display`. The button screens (3/4) pin it near the top at root `pad_top 30` + title `pad_top 36` = **66 px from top**.
- **Subtext** uses the Welcome-screen style — secondary colour, `font_title` (26 px) — for consistency ("Your desk deserves better").
- **Tertiary Close buttons** land at the same position as the Welcome **Start** button (`make_base(button_screen=true)` reserves the bottom room).

### Installing screen specifics (screen 2)

`set_installing()` transforms the live download screen in place (seamless Updating→Installing transition):
- **Title** retitled "Installing firmware", pinned at the same Y as the download title (`LV_OBJ_FLAG_IGNORE_LAYOUT` so hiding the ring doesn't shift it).
- **Progress ring hidden** — no live percentage during the flash commit.
- **Instruction text** ("Screen goes dark for a few seconds — keep Ori powered. It restarts when done.") pinned to screen centre, independent of the title.
- **Countdown bar** at the bottom: 6 px accent strip filling 0→100 over `linger_ms`, mirroring the Setup-Complete countdown bar.

### Behaviour

- All touch inert — mode-toggle and long-press triggers do nothing.
- **Meeting-check tick paused** for the whole update (`lv_timer_pause()` in `on_ota_begin`), resumed in `on_reconnect_end` only on failure (success reboots). Other timers are screen-local and cleaned up when the OTA screen replaces the runtime screen.
- **Transfer quiet mode for the whole download phase** (`ble_manager::set_ota_transfer_quiet(true)` at accepted BEGIN). Over USB this existed to keep BLE bursts off the serial RX path; over BLE the motivation is sharper — everything it silences now competes for the *same radio* carrying the image:
  - **Advertising stops** and `restart_advertising()` no-ops for the duration — a bonded peer reconnecting mid-download fires the heaviest BLE burst in the system (encryption restore → NimBLE's bonding-restored CCCD replay → auto `onSubscribe` → ANCS resync relay → Orion delta sync), plus bond/CCCD NVS writes whose cache-disable stalls both cores. Orion's own link is exempt by construction: it is already connected, and it is the transport.
  - **ANCS processing suspended at the source** (`ancs_client::suspend_for_ota()`): the NS/DS notify callbacks drop incoming events instead of ringing them (per-notification attribute fetches + icon lookups are the heaviest BLE-triggered CPU work), and `ancs_client::poll()` skips its whole body (CTS read, RSSI HCI round-trip, drains).
  - **The iPhone link is dropped outright** (new for BLE transport). Suspending ANCS stops the CPU work, but an idle second connection still costs radio time — the controller must schedule its connection events in the same timeline as Orion's, which is exactly the budget the transfer is trying to saturate. One connection gets every event. The phone reconnects on its own once advertising re-arms (or after the reboot), and a fresh connection is what makes iOS replay the notification backlog anyway (`setup-flow.md`).
  - **On failure-resume** (`show_error_screen` → `set_ota_transfer_quiet(false)`): the connection interval goes back to 15–30 ms, advertising re-arms per bond state, and ANCS resumes. The force-drop-to-replay-backlog step is usually a no-op now, since the phone link is already down. Pre-accept rejects (`too_large`/`no_memory`) never entered quiet mode; the lift is a no-op there. A successful commit never lifts it — the reboot supersedes.
- **Connection interval boosted** to 7.5–15 ms for the transfer (`ble_manager::set_ota_link_fast(true)`), restored to 15–30 ms on failure. Best-effort: a central that declines just makes the transfer slower.
- **BLE writes NACKed** for all data characteristics while `is_active()` — *except* chars `0014`/`0015` themselves, which are exempt by construction (they are the update). Orion retries the rest after reboot.
- **`Keyboard Command` notifies suspended** for the duration.
- Download phase is interruptible: losing the link discards the partial image (`FAILED link_lost`), and there is no cross-connection resume — restart from `BEGIN`. Once the image verifies and the Installing linger starts, the commit runs through to reboot regardless of the link.
- **A retry supersedes an undismissed error screen.** A `BEGIN` arriving while Ori sits on "Update failed" is accepted, not rejected as `busy` — requiring someone to walk over and tap Close made sense when they had to walk over to plug a cable in anyway.

### Serial test commands (`ORI_DEBUG_SERIAL`)

`screen_manager.cpp` exposes the screens for hand-testing without a real transfer: `u` Updating · `1` Installing (screen goes dark, with the countdown bar) · `2` Updated ack · `3` Update failed.

## When an update may not start

Ori rejects `BEGIN` with `REJECT { r }` when:

- a transfer is already in progress (`"busy"`) — a *failed* one that hasn't been dismissed does not count
- `v`, `n`, or `h` is missing (`"missing_fields"`) — `v` is **required** (it's one half of the version consistency check below)
- the payload isn't a valid CBOR map (`"cbor_decode"` / `"not_map"`), or `o` isn't a known op (`"bad_op"`)
- `n` exceeds inactive slot capacity (`"too_large"`)
- the PSRAM staging buffer won't allocate (`"no_memory"`)

There is **no countdown guard** — the update is user-initiated in Orion, so the user's explicit intent overrides the 5-minute pre-meeting alert. Once accepted, OTA is the top-priority state (`state-machine.md`), so it takes over the countdown modal and the alert is suppressed while the update runs.

After the full image is staged and its hash verified, Ori fails with `FAILED { "version_mismatch" }` if the version stamped **inside the binary** does not match the `v` Orion declared at `BEGIN` (or if the image has no version marker). A structurally invalid image (header magic ≠ `0xE9`) fails with `FAILED { "bad_image" }`.

**An update requires the Orion bond.** This is stricter than the old USB path, whose authority was physical cable access. Both firmware-update characteristics are MITM-encrypted, and Ori additionally requires the writer to be the bonded Orion peer — so the iPhone bond, which exists only so Ori can read ANCS as a client, can never push firmware.

## Version & rollback policy

- **The only version rejection is claim-vs-binary mismatch.** Compares two inputs, rejects only when they DISAGREE:
  1. *BEGIN claim:* the `v` string Orion declares at `BEGIN` (required; stored as `g_claimed_version`).
  2. *Binary truth:* every Ori image embeds a contiguous marker `"OriFwVer=<version>"` (= `ORI_FW_VERSION`). After the hash verifies, `ota_receiver` scans the staged PSRAM image for it (`g_ori_fw_marker` + `extract_image_version()` in `firmware/src/ota_receiver.cpp`).

  Mismatch or absent marker → `FAILED { "version_mismatch" }`. Otherwise the update proceeds and the binary's version shows on the post-reboot ack. Consistency/integrity check only — does NOT block any particular version.
  - The marker must be a single referenced `char[]` literal (e.g. logged in `ota_receiver::init()`), or `-O2` ICF/`--gc-sections` will split or drop it. The scanner's search pattern is XOR-obfuscated so the scanner's own copy can't be a false match.
  - Not `esp_app_desc.version`: on this precompiled Arduino core it's the core's git hash, not ours.
- **Downgrades and same-version re-installs are allowed.** There is **no** `already_current` reject and **no** semver "is-newer" gate. Re-flashing the running version, or an older one, is permitted as long as the BEGIN claim matches the binary. Do not add anti-rollback enforcement (eFuse secure-version, etc.).
- **Integrity, not authenticity.** The `sha256` check proves the bytes are intact; the binary marker proves the label matches the binary; the bond proves the sender is the paired Orion. None of that proves the *image* came from Orinari — a malicious binary can stamp any version and a matching claim. True authenticity needs Secure Boot.
- **Firmware signing**: if/when "only our firmware runs" is enforced, the chosen path is **ESP32-S3 Secure Boot v2 (+ Flash Encryption)** at factory provisioning — *not* an app-level signature check in the OTA receiver (a UART reflash bypasses app-level checks). Deferred to M8. Configure Secure Boot **without** anti-rollback, consistent with the downgrade-allowed policy above.
  - This matters more than it did over USB. The old transport's implicit gate was "you are holding the device"; the new one's is "you are the bonded PC," which is a real gate but a remote one.

## Bricked-unit recovery

If the unit won't boot far enough to advertise, or BLE itself is broken by the shipped build: open the enclosure, connect the internal UART port, reflash with `esptool.py write_flash`. Service / RMA path only — no port is customer-accessible. See `hardware.md`.

**This is the one real cost of dropping USB CDC.** A build that boots but has broken BLE cannot be updated over BLE, and there is no second wireless channel. The mitigations are the ones already in place: the bootloader's 60 s pending-verify window rolls back a build that panics or watchdogs, and the inactive slot is never touched until an image has been fully staged and verified. A build that boots *healthily* and merely has a BLE regression is the gap — it survives pending-verify and then can't be reached. Treat "BLE comes up and Orion can connect" as a release-blocking smoke test on real hardware before every published build.

## Failure modes

| Failure | Behavior |
|---|---|
| Link drops mid-transfer | `FAILED { "link_lost" }` on Ori (or `"ble_timeout"` if the drop isn't detected first); partial image discarded; Orion restarts from `BEGIN` |
| Sender stops sending | No-progress watchdog fires after 10 s → `FAILED { "ble_timeout" }` |
| Dropped fragment | Ori notifies `RESUME { b }`; sender rewinds to that offset and continues. Not a failure |
| SHA-256 mismatch | `FAILED { "hash_mismatch" }`; image discarded; resume runtime |
| Size overflow | `FAILED { "size_overflow" }` mid-stream |
| Declared size too large | `REJECT { "too_large" }` at BEGIN |
| User cancels in Orion | `ABORT` → `FAILED { "aborted" }`; resume runtime |
| Flash write fails | No frame (BLE already down). Device reboots into the old firmware; Orion sees an unchanged version |
| Unhealthy first boot | Auto-rollback on next boot; Orion can re-attempt |
| Ori not connected | Orion prompts "Ori isn't connected. Make sure it's powered and in range, then try again." |

---

## Orion (sender) implementation guide

This is the end-to-end algorithm `orion-sync` implements to push an update. Authoritative how-to for the host side; the firmware side is in `ota_receiver.cpp`. A working reference sender lives in **`tools/mock_orion_ble_ota.py`**, which scripts 11 scenarios (happy path, link drop, wrong version, oversized image, corrupted hash, truncated transfer, declared-size overflow, concurrent BEGIN, malformed BEGIN, dropped-fragment resync, abort) against the reason table in step 6 — a useful checklist for Orion's error handling.

### 0. Read the .bin (before touching BLE)

1. **Extract the firmware version from the binary** — never hardcode/guess. Scan for the ASCII marker `OriFwVer=` and read the null-terminated version after it (e.g. `OriFwVer=1.2.3\0` → `"1.2.3"`) — this is the `v` sent in `BEGIN`; the device re-reads the same marker from the staged image and **fails with `version_mismatch` if Orion's claim disagrees**. Absent marker = not a valid Ori image — abort.
   ```
   i = bytes.find(b"OriFwVer="); version = bytes[i+9 : bytes.index(0, i+9)]
   ```
2. `n` (total_size) = file length in bytes.
3. `h` (sha256) = SHA-256 over the whole file, as a CBOR byte string.
4. Optional sanity: first byte should be `0xE9` (ESP32 app image magic) — the device also checks this (`bad_image`).

### 1. Have a live Orion link

- The update rides the existing bonded, encrypted connection. If Ori isn't currently connected, don't try to force it — surface *"Ori isn't connected. Make sure it's powered and in range, then try again."*
- Subscribe to the Control characteristic (`0014`) **before** writing `BEGIN`, or the `READY` notify is lost. (Same rule the ANCS relay chars already live under — `firmware.md`.)
- Do not start an update while a sync session is open; finish or let it end first.

### 2. BEGIN handshake

- Write Control `{ "o": "BEGIN", "v": <version from the binary>, "n": <size>, "h": <sha256> }`, with response.
- Wait up to ~15 s for a notify:
  - `READY` → proceed to streaming.
  - `REJECT { r }` → stop; surface the reason — see the table in step 6.
  - timeout → link or subscription problem; surface "couldn't talk to Ori", retry.

### 3. Stream Data with windowed flow control

Fragments go to char `0015` as `[offset uint32 LE][payload]`, `payload ≤ ATT_MTU - 3 - 4` (240 B at MTU 247).

- **Stream Write-No-Response.** This is the dominant throughput lever — a per-fragment ATT round-trip would cap you near one fragment per connection event.
- **Checkpoint every ~24 fragments, and on the last one, with a Write-with-response.** Its ATT ack proves the whole preceding burst landed and blocks the sender until Ori has drained it. Without this the controller accepts more than NimBLE can process and fragments are silently dropped. (Same technique §5 specifies for the bulk sync writes.)
- **Honour `RESUME { b }` immediately.** It means Ori's next expected offset is `b` — seek there and continue. Don't treat it as an error and don't restart from zero.
- Drive the host-side progress bar from `PROGRESS { b }`.
- If `FAILED { r }` arrives mid-stream, stop.
- Ori's no-progress watchdog is 10 s; don't leave gaps that long.

### 4. END → automatic install

- Write Control `{ "o": "END" }` after the last data frame's checkpoint ack.
- The device installs **automatically** (no user action): hash check → version check → ~3.5 s "Installing firmware" frame → flash commit (screen dark a few seconds) → reboot.
- Wait up to ~60 s for `VALIDATED { v }` or `FAILED { r }`.
- On `VALIDATED`: **expect the link to drop** — the BLE stack is torn down for the commit. Treat the disconnect as normal, reconnect once Ori re-advertises, and confirm the new version via the Firmware Revision String characteristic. That read, not the `VALIDATED` frame, is what proves the install landed.

### 5. Cancelling

- Write Control `{ "o": "ABORT" }`. Ori discards the staged image and shows its error screen with "The update was cancelled."
- Only meaningful during the download. Once `VALIDATED` is out, the install completes regardless.

### 6. Error tracking — reasons and Orion's response

| Signal | Cause | Orion action |
|---|---|---|
| Ori not connected | out of range / powered off | Prompt to bring it in range and retry |
| no `READY` after `BEGIN` | not subscribed, or link problem | Check the subscription, surface "couldn't talk to Ori", retry |
| `REJECT { too_large }` | image > 3 MB OTA slot | Build/config error — block the update |
| `REJECT { missing_fields }` | BEGIN missing v/n/h | Orion bug — fix the BEGIN payload |
| `REJECT { cbor_decode }` / `REJECT { not_map }` / `REJECT { bad_op }` | BEGIN payload isn't a valid CBOR map, or an unknown op | Orion bug — fix the BEGIN encoding |
| `REJECT { no_memory }` | device couldn't allocate a PSRAM staging buffer for `n` | Device also shows an on-screen error; surface "Ori is low on memory, try again" and retry |
| `REJECT { busy }` | a second `BEGIN` arrived while a transfer is live | Orion bug (one BEGIN per session) — wait for the active transfer to finish/fail, don't restart it |
| `RESUME { b }` | a fragment was dropped in flight | **Not an error** — rewind to `b` and keep going |
| `FAILED { version_mismatch }` | Orion's `v` claim ≠ the version in the binary | Orion bug — send the version extracted from the .bin (step 0); or wrong file |
| `FAILED { bad_image }` | not a valid ESP32 app (magic ≠ 0xE9) | Wrong/corrupt file — pick the right .bin |
| `FAILED { hash_mismatch }` | bytes corrupted in transfer | Retry from `BEGIN` (no resume) |
| `FAILED { truncated }` | END sent before all declared bytes arrived | Streaming bug — retry from `BEGIN` |
| `FAILED { size_overflow }` | more bytes sent than declared | Streaming bug — retry from `BEGIN` |
| `FAILED { link_lost }` / `{ ble_timeout }` | connection dropped or stalled mid-stream | Surface "connection lost during update"; retry from `BEGIN` |
| `FAILED { aborted }` | Orion sent ABORT | Expected — return to Settings |
| disconnect after `VALIDATED`, version unchanged on reconnect | flash write failed; device booted the old slot | Surface "the update didn't install"; retry |

**General rules:** every failure is recoverable by restarting from `BEGIN` (there is no cross-connection resume; `RESUME` is within-session only). Use timeouts on each phase (BEGIN→READY ~15 s; stream stall ~20 s; END→VALIDATED ~60 s). The bonded Orion link is the only authority — there is no physical-access path any more.
