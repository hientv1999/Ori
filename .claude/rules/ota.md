# Ori — Firmware Update

Firmware updates run over **USB CDC** on the existing USB-C power cable. No BLE OTA, no USB-MSC (Ori must not appear as a removable drive). Bricked-unit recovery uses the internal UART port — service path only.

> **On the word "OTA":** the code uses "OTA" (file names, the `OTA_UPDATING` state, the `ota_0`/`ota_1` partitions) because that's the ESP-IDF term for a *slot-swap firmware update* — **not** because the transport is wireless. Ori's transport is wired USB CDC. User-facing copy always says "firmware update," never "OTA."

| Property | Value |
|---|---|
| Initiator | Orion (Settings → "Install update") |
| Transport | USB CDC over the existing USB-C power cable |
| Time for ~1.5 MB image | ~10–30 s |
| Driver requirement | None — native USB CDC on Win 10+, macOS, Linux |
| Visibility in file managers | None — enumerates as COM/tty, never as a drive |

## Firmware implementation

- Arduino **`Update`** library: `Update.begin()`, `Update.write(buf, len)`, `Update.end(true)`.
- `Update.canRollBack()` / `Update.rollBack()` for the post-boot verify path.
- Two 3 MB OTA slots (`ota_0`, `ota_1`) + OTA-data partition — see `firmware/partitions.csv`.
- Pending-verify window: **60 seconds** after first boot. If new firmware fails (panic, watchdog, no USB/BLE), the bootloader rolls back automatically on next boot.

## Transport

- ESP32-S3 native USB peripheral; no USB-to-serial chip. Already enabled: `-D ARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`.
- Orion (one Rust codebase, both platforms — `memory.md`) uses the `serialport` crate, which wraps the same native APIs a platform-specific implementation would (Windows serial APIs / macOS IOKit). Driverless on Win 10+ and macOS.
- The same CDC port carries firmware boot logs (`Serial.printf`). The framing protocol below separates log bytes from OTA frames.

## Wire flow

```
Orion checks ori.app for latest version vs fw_version read from the standard
Firmware Revision String characteristic (Device Information Service, BLE).
If newer exists:
   Orion UI: Settings → "Update available · 1.2.3" → tap "Install update"

Orion opens the serial port to the USB-C-connected Ori.
   If no port found: prompt "Plug Ori into this PC, then try again."

Orion writes OTA frame: BEGIN { fw_version, total_size, sha256 }

Ori validates: required fields present; size <= inactive slot capacity; PSRAM staging buffer allocates; no transfer already in progress.
   On reject: respond REJECT { reason }; return to normal runtime.
   On accept: Update.begin(inactive_slot, total_size);
              switch screen to OTA-Updating;
              respond READY.

Orion streams DATA frames. Firmware feeds each into Update.write().
Every ~5% of total_size, Ori responds PROGRESS { bytes_received }.
After last DATA, Orion writes END.

Ori computes SHA-256 over the received image.
   On match:    Update.end(true); respond VALIDATED; sleep 200 ms; ESP.restart().
   On mismatch: Update.abort();   respond FAILED { "hash_mismatch" }; resume runtime.

After reboot:
   Bootloader runs new firmware in pending-verify state.
   Firmware enumerates USB, brings up BLE, advertises to bonded peers.
   60 s healthy → mark partition valid.
   60 s unhealthy (panic / watchdog) → bootloader rolls back on next boot.

Orion reconnects over BLE, reads the Firmware Revision String characteristic,
confirms new fw_version.
```

## Framing (USB CDC OTA protocol)

Each frame:

```
Offset  Size  Field
0       2     magic       = 0x4F54 ("OT" — distinguishes OTA frames from log bytes)
2       1     op
3       3     payload_len (uint24, little-endian)
6       N     payload     (CBOR for control ops; raw bytes for DATA)
```

Log bytes from `Serial.printf` never start with `0x4F54`; Orion surfaces them as boot-log output.

```
op  Name        Direction  Payload
01  BEGIN       PC → Ori   CBOR: { fw_version: text, total_size: uint, sha256: bytes(32) }
02  READY       Ori → PC   CBOR: {}
03  REJECT      Ori → PC   CBOR: { reason: text }
04  DATA        PC → Ori   raw bytes
05  PROGRESS    Ori → PC   CBOR: { bytes_received: uint }
06  END         PC → Ori   CBOR: {}
07  VALIDATED   Ori → PC   CBOR: {}
08  FAILED      Ori → PC   CBOR: { reason: text }
```

DATA has no per-frame sequence numbers — CDC delivers in order; firmware counts bytes against `total_size`.

## On-device UX during update

All OTA screens are full-screen takeovers (status bar + profile card + left panel hidden) built in `firmware/src/screens/screen_ota_updating.cpp`. The flow is **automatic**: once the image is downloaded and verified, the device advances straight to the Installing frame and commits after a short linger — **no user confirmation step**.

### Screen sequence

| # | Screen | Builder | Content | Notes |
|---|---|---|---|---|
| 1 | **Updating firmware** | `create()` | Title + live progress ring (220 px, % in centre) + "Keep Ori plugged in" | Ring driven by `set_progress()` on each PROGRESS frame; image streams into PSRAM (LCD stays live). State `AwaitingData`. |
| 2 | **Installing** | `set_installing(linger_ms)` | "Installing firmware" title + centred instruction + bottom countdown bar | Reached automatically at END after the hash + version check pass (state `Installing`). Reuses screen 1's objects (see layout below). Shown for `linger_ms` (= `COMMIT_LINGER_MS`, 3.5 s), then the LCD halts for the flash burst. |
| — | *(commit)* | — | Screen **dark** | LCD halted (PSRAM-DMA vs flash MSPI contention), BLE quiesced, image written to flash, then reboot. No frame can be shown between flash and reboot. |
| 3 | **Firmware updated** | `create_updated_ack(version, on_close)` | "Firmware updated" title + "Ori is now running version X" + animated check + **Close** tertiary button | Post-reboot acknowledgement. **Persisted in NVS** (`ota_ack` key) — reappears on every boot until the user taps Close. Serves as the completion confirmation (there is no separate "Update complete / Restart" screen — the install→reboot is atomic). |
| 4 | **Update failed** | `create_error(message, on_close)` | "Update failed" title + warning glyph + plain-language reason + **Close** tertiary button | Shown for in-flight failures (usb_timeout / truncated / hash_mismatch / size_overflow / flash_error) and user-relevant BEGIN rejects (too_large / no_memory / version_mismatch / bad_image). Wire still carries the terse code; `friendly_reason()` maps it to the on-screen sentence. |

There is **no "Firmware Install / Update now" gate** — the download flows straight into the install. (Removed along with `create_ready`, the `AwaitingConfirm` state, and the confirm watchdog.)

### Shared layout conventions

- **Title** uses `font_display`. The button screens (3/4) pin it near the top at root `pad_top 30` + title `pad_top 36` = **66 px from top**.
- **Subtext** uses the Welcome-screen style — secondary colour, `font_title` (26 px) — for consistency ("Your desk deserves better").
- **Tertiary Close buttons** land at the same position as the Welcome **Start** button (`make_base(button_screen=true)` reserves the bottom room).

### Installing screen specifics (screen 2)

`set_installing()` transforms the live download screen in place rather than building a new one, so the Updating→Installing transition is seamless:
- **Title** retitled "Installing firmware" and pinned at the **same Y as the "Updating firmware" title** — the download title's laid-out Y is captured (`lv_obj_get_y` after `lv_obj_update_layout`) and the title is pinned there with `LV_OBJ_FLAG_IGNORE_LAYOUT` so hiding the ring doesn't shift it.
- **Progress ring hidden** (`LV_OBJ_FLAG_HIDDEN`) — there's no live percentage during the flash commit.
- **Instruction text** ("Screen goes dark for a few seconds — keep Ori plugged in. It restarts when done.") is taken out of the flex flow (`LV_OBJ_FLAG_IGNORE_LAYOUT`) and pinned to the **centre of the screen**, independent of the top title.
- **Countdown bar** at the very bottom: a **6 px** (2× the 3 px Setup-Complete bar) accent strip (`COLOR_ELEV` track, `COLOR_ACCENT` indicator) that fills 0→100 linearly over `linger_ms`, so the user can see how long until the screen blanks. Mirrors the Setup-Complete countdown bar; created on the top-level screen and cleaned up with it.

### Behaviour

- All touch inert — mode-toggle and long-press triggers do nothing.
- **Meeting-check tick paused.** The 1 s state-machine tick (meeting expiry / 5-minute alert / `evaluate()`) is paused for the whole update via `lv_timer_pause()` in `on_ota_begin`, resumed in `on_reconnect_end` only if the update fails (success reboots). No meeting/alert logic runs or touches state while the OTA owns the device. (Other timers are screen-local — the status bar, profile card, and any countdown modal are deleted when the OTA screen replaces the runtime screen, cleaning up their timers.)
- **BLE quiesced before the flash commit** (`ble_manager::quiesce_for_commit()` — stops advertising + `NimBLEDevice::deinit`), so nothing BLE-side executes or triggers an NVS/flash write while `Update.write()` has the cache disabled.
- **BLE writes NACKed** for all data characteristics (Profile Info, Photo, Meeting List, Time Off Entry, Factory Reset Command) while `is_active()` — Orion retries after reboot.
- **`Keyboard Command` notifies suspended** for the duration.
- Download phase is interruptible (unplug = partial image discarded, active slot untouched, no resume — Orion restarts from `BEGIN`). Once the image verifies and the Installing linger starts, the commit runs through to reboot.

### Serial test commands (`ORI_DEBUG_SERIAL`)

`screen_manager.cpp` exposes the screens for hand-testing without a real transfer: `u` Updating · `1` Installing (screen goes dark, with the countdown bar) · `2` Updated ack · `3` Update failed.

## When an update may not start

Ori rejects `BEGIN` with `REJECT { reason }` when:

- `total_size` exceeds inactive slot capacity (`"too_large"`)
- `fw_version`, `total_size`, or `sha256` is missing (`"missing_fields"`) — `fw_version` is **required** (it's one half of the version consistency check below)

There is **no countdown guard** — the update is user-initiated in Orion, so the user's explicit intent overrides the 5-minute pre-meeting alert. Once accepted, OTA is the top-priority state (`state-machine.md`), so it takes over the countdown modal and the alert is suppressed while the update runs.

After the full image is staged and its hash verified, Ori fails with `FAILED { "version_mismatch" }` if the version stamped **inside the binary** does not match the `fw_version` Orion declared at `BEGIN` (or if the image has no version marker). A structurally invalid image (header magic ≠ `0xE9`) fails with `FAILED { "bad_image" }`.

USB CDC OTA does **not** require a BLE bond — physical cable access is sufficient authority.

## Version & rollback policy

- **The only version rejection is claim-vs-binary mismatch.** The version check compares two inputs and rejects only when they DISAGREE:
  1. *BEGIN claim:* the `fw_version` string Orion declares at `BEGIN` (required; stored as `g_claimed_version`).
  2. *Binary truth:* every Ori image embeds a contiguous marker `"OriFwVer=<version>"` (= `ORI_FW_VERSION`). After the hash verifies, `ota_receiver` scans the staged PSRAM image for this marker (`g_ori_fw_marker` + `extract_image_version()` in `firmware/src/ota_receiver.cpp`).

  If the binary's version ≠ the BEGIN claim (or the marker is absent → can't verify) → `FAILED { "version_mismatch" }`. Otherwise the update proceeds and the binary's version is shown on the post-reboot ack. This is a **consistency/integrity** check that Orion labelled the image honestly — it does NOT block any particular version.
  - The marker must be a single `char[]` (one string literal) and must be *referenced* somewhere (e.g. the boot log in `ota_receiver::init()`), or `-O2` ICF / `--gc-sections` will split or drop it. The scanner's search pattern is XOR-obfuscated so the scanner's own copy can't be a false match.
  - Why not `esp_app_desc.version`: on this precompiled Arduino core it's the core's git hash (`arduino-lib-builder`), not ours.
- **Downgrades and same-version re-installs are allowed.** There is **no** `already_current` reject and **no** semver "is-newer" gate. Re-flashing the running version, or an older one, is permitted as long as the BEGIN claim matches the binary. Do not add anti-rollback enforcement (eFuse secure-version, etc.).
- **Integrity, not authenticity.** The `sha256` check proves the bytes are intact; the binary marker proves the label matches the binary. Neither proves origin — a malicious binary can stamp any version and a matching claim. True authenticity needs Secure Boot.
- **Firmware signing**: if/when "only our firmware runs" is enforced, the chosen path is **ESP32-S3 Secure Boot v2 (+ Flash Encryption)** at factory provisioning — *not* an app-level signature check in the OTA receiver (a UART reflash bypasses app-level checks). Deferred to M8. Configure Secure Boot **without** anti-rollback, consistent with the downgrade-allowed policy above.

## Bricked-unit recovery

If USB CDC won't enumerate or won't respond to `BEGIN`: open the enclosure, connect the internal UART port, reflash with `esptool.py write_flash`. Service / RMA path only — UART is not customer-accessible. See `hardware.md`.

## Failure modes

| Failure | Behavior |
|---|---|
| USB unplugged mid-transfer | Partial image discarded; Orion restarts from `BEGIN` |
| SHA-256 mismatch | `FAILED { "hash_mismatch" }`; image discarded; resume runtime |
| Size overflow | `REJECT { "too_large" }` at BEGIN |
| Unhealthy first boot | Auto-rollback on next boot; Orion can re-attempt |
| Serial port not found | Orion prompts "Plug Ori into this PC, then try again." |

---

## Orion (sender) implementation guide

This is the end-to-end algorithm `orion-sync` implements to push an update. It is the authoritative how-to for the host side; the firmware side is in `ota_receiver.cpp`. A working reference sender lives in `tools/mock_orion_ota.py`, which also scripts 9 failure-mode scenarios (broken cable / wrong version / oversized image / corrupted hash / truncated transfer / declared-size overflow / concurrent BEGIN / malformed BEGIN, plus the happy path) against the reason table in step 6 below — useful as an end-to-end checklist for Orion's error handling.

### 0. Read the .bin (before opening the port)

1. **Extract the firmware version from the binary** — do NOT hardcode or guess it. Scan the image bytes for the ASCII marker `OriFwVer=` and read the null-terminated version that follows (e.g. `OriFwVer=1.2.3\0` → `"1.2.3"`). This is the value sent as `fw_version` in `BEGIN`; the device re-reads the *same* marker from the staged image and **rejects with `version_mismatch` if Orion's claim disagrees**. If the marker is absent, the file is not a valid Ori image — abort with a clear error.
   ```
   i = bytes.find(b"OriFwVer="); version = bytes[i+9 : bytes.index(0, i+9)]
   ```
2. `total_size` = file length in bytes.
3. `sha256` = SHA-256 over the whole file.
4. Optional sanity: first byte should be `0xE9` (ESP32 app image magic) — the device also checks this (`bad_image`).

### 1. Open the port

- Find Ori's CDC port (ESP32-S3 native USB, VID `0x303A`). If none: prompt *"Plug Ori into this PC, then try again."* If more than one port matches the VID (e.g. another ESP32-S3 dev board attached), don't guess — list the candidates and ask the user to pick one.
- Open it and **assert DTR = true** (HWCDC only transmits when DTR is asserted), RTS = false. Baud is irrelevant over USB CDC.

### 2. Frame reader — robust to log noise (production vs dev)

Every device→host response is a framed message (`§ Framing`): magic `0x4F54`, op, uint24-LE length, payload. The reader must:

- **Scan for the magic `0x4F54`**, then validate `op` is a known response op AND `payload_len ≤ a sane cap (e.g. 4 KB)` before accepting — this skips false-positive `"OT"` byte pairs.
- **Never parse log text for state.** All state comes from frames (`READY` / `PROGRESS` / `VALIDATED` / `FAILED` / `REJECT`). Logs are diagnostics only.

> **Production vs development — important.** In production the firmware's USB logging is **disabled**, so the CDC port carries **only OTA frames** (a clean stream). In development (`ORI_DEBUG_SERIAL` / `Serial.printf`) the port also carries log text and boot logs interleaved between frames. The same magic-scanning reader handles **both**: it ignores the (absent-in-production) log bytes and locks onto frames. Two rules follow:
> - Orion must **not depend on any log line** being present (there are none in production) — e.g. confirm the post-update version over BLE (Firmware Revision String characteristic), never by reading a boot-log string.
> - Orion must **not be confused by** log bytes when they *are* present (dev) — hence the magic-scan + validation. Device→host frames are written atomically by the firmware, so a log line can only appear *between* frames, never inside one.

### 3. BEGIN handshake

- Send `BEGIN { fw_version (from the binary), total_size, sha256 }`.
- Wait up to ~10 s for a response:
  - `READY` → proceed to streaming.
  - `REJECT { reason }` → stop; surface the reason — see the reason table in step 6. (There is **no** `countdown_active` reject — a pending meeting never blocks the update.)
  - timeout → comm error; close and prompt to retry.

### 4. Stream DATA with windowed flow control

The device RX buffer is 32 KB and it acks via `PROGRESS { bytes_received }` every ≤ 8 KB. Orion must never run more than the buffer ahead of the acks:

- Keep `(bytes_sent − bytes_acked) ≤ WINDOW` (16 KB works well). Send `DATA` frames (raw bytes, e.g. 4 KB each); after each send, drain any response frames and update `bytes_acked` from `PROGRESS`.
- **Keep draining while you send, not just between phases.** Every device→host frame — including `PROGRESS` — is followed by a blocking `Serial.flush()` on the firmware side (`ota_receiver.cpp`'s `send_frame()`). If Orion only reads responses after it finishes writing a batch of `DATA`, both ends can sit waiting on a full buffer at once; draining inside the same loop that sends `DATA` (as the window check above already requires) avoids this.
- Drive the host-side progress bar from `PROGRESS`.
- If a `FAILED { size_overflow }` arrives mid-stream, stop.

### 5. END → automatic install

- Send `END`.
- The device installs **automatically** (no user action): hash check → version check → ~3.5 s "Installing firmware" frame → flash commit (screen dark a few seconds) → `VALIDATED` → reboot.
- Wait up to ~**45 s** for `VALIDATED` (covers the linger + commit) or `FAILED { reason }`.
- On `VALIDATED`: the device reboots and the COM/tty port **re-enumerates**. Close the port; after re-enumeration, confirm the new `fw_version` over BLE (Firmware Revision String characteristic). Do not expect any serial output post-reboot in production.

### 6. Error tracking — reasons and Orion's response

| Signal | Cause | Orion action |
|---|---|---|
| no port found | Ori not plugged into this PC | Prompt to plug in and retry |
| no `READY` after `BEGIN` | comm/port issue, wrong device | Close, surface "couldn't talk to Ori", retry |
| `REJECT { too_large }` | image > 3 MB OTA slot | Build/config error — block the update |
| `REJECT { missing_fields }` | BEGIN missing fw_version/size/sha | Orion bug — fix the BEGIN payload |
| `REJECT { cbor_decode }` / `REJECT { not_map }` | BEGIN payload isn't a valid CBOR map | Orion bug — fix the BEGIN encoding |
| `REJECT { no_memory }` | device couldn't allocate a PSRAM staging buffer for `total_size` | Device also shows an on-screen error; surface "Ori is low on memory, try again" and retry |
| `REJECT { busy }` | a second `BEGIN` arrived while a transfer is already in progress | Orion bug (one BEGIN per session) — wait for the active transfer to finish/fail, don't restart it |
| `FAILED { version_mismatch }` | Orion's `fw_version` claim ≠ the version in the binary | Orion bug — send the version extracted from the .bin (step 0); or wrong file |
| `FAILED { bad_image }` | not a valid ESP32 app (magic ≠ 0xE9) | Wrong/corrupt file — pick the right .bin |
| `FAILED { hash_mismatch }` | bytes corrupted in transfer | Retry from `BEGIN` (no resume) |
| `FAILED { truncated }` / `size_overflow` | byte-count mismatch | Streaming bug — retry from `BEGIN` |
| `FAILED { flash_error }` | device flash write failed | Device reboots to old firmware; retry |
| `FAILED { usb_timeout }`, no `VALIDATED`, port goes quiet | cable pulled / host stalled mid-stream → device stall-aborts after 3 s and resumes runtime | Surface "connection lost during update"; retry from `BEGIN` |

**General rules:** every failure is recoverable by restarting from `BEGIN` (there is no resume). Use timeouts on each phase (BEGIN→READY ~10 s; stream stall; END→VALIDATED ~45 s). Physical cable access is the only authority — no BLE bond required (`§ Transport`).
