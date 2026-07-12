# Ori — Mass-Production Identity Provisioning

Every Ori unit gets two facts burned in at manufacturing time: a **serial
number** and a **manufacture date**. Both must survive everything short of a
chip-level reflash of the whole device — a firmware update, a factory reset,
even years of ordinary use — because they're the basis for warranty/RMA
lookups and any future "which batch was this" investigation. This document
is the authoritative description of how that data is stored, written, and
exposed. Firmware side: `esp32-connectivity`/`firmware-core`. Manufacturing
tooling: whoever runs the flashing station.

---

## 1. Why a separate NVS partition

Ori already has one NVS partition ("nvs", `firmware/partitions.csv`) holding
everything from the profile card to BLE bond addresses, and `nvs::factory_reset()`
(`firmware/src/nvs_store.cpp`) wipes it wholesale via `Preferences::clear()`
on every factory reset. Serial number and manufacture date must NOT be in
that blast radius.

Two ways to achieve that were considered:

1. **A different NVS *namespace* inside the same "nvs" partition.** Cheaper
   (no partition table change), and would already survive today's
   `factory_reset()` — it only clears the `"ori"` namespace, not the whole
   partition.
2. **A physically separate NVS *partition*.** (Chosen.) Immune not just to
   today's `factory_reset()` but to any *future* mistake — a debug build that
   calls `nvs_flash_erase()` on the whole partition, a "wipe everything and
   start over" troubleshooting step, a copy-paste of the wrong namespace
   string. Erasing a partition requires naming that partition explicitly, so
   there is no code path anywhere in this firmware that can touch it by
   accident.

Option 2 costs one thing: 16 KB of flash, carved out of the ~9.8 MB `spiffs`
partition (which has ample headroom — see `firmware/partitions.csv`'s own
note on shrinking `spiffs` being a known-safe, previously-done operation, for
the `coredump` partition). Given how expensive getting this wrong would be
(re-flashing an already-boxed unit, or worse, permanently losing its
identity), the extra 16 KB is the right trade.

```
# firmware/partitions.csv
factory,   data, nvs,      0xFEC000, 0x4000,   # 16 KB, see below
```

- **Type/subtype**: plain `nvs` — same format as the main partition, just a
  second instance. Opened via `Preferences::begin(name, readOnly, "factory")`
  (the Arduino `Preferences` library's optional third argument selects a
  named partition instead of the default one — `firmware/src/factory_info.cpp`).
- **Namespace inside it**: `"factory"`, keys `"sn"` (serial number) and
  `"mfg"` (manufacture date, ISO-8601 `"YYYY-MM-DD"`).
- **Firmware never writes to it.** `factory_info.h` exposes only
  `serial_number()`/`manufacture_date()` read accessors — there is no setter
  anywhere in the firmware, over BLE, USB CDC, or otherwise. The only writer
  is the one-time manufacturing flash described below. This is deliberate:
  the smallest possible attack/bug surface for data that's supposed to be
  permanent is "the firmware can't touch it at all."
- **Survives an OTA update** for a more basic reason: USB CDC firmware
  updates (`ota.md`) only ever write the `ota_0`/`ota_1` app-slot partitions.
  Every NVS partition — main or factory — is untouched by that process by
  construction, the same reason bonds/profile already survive a firmware
  update today.

## 2. Serial number format

```
ORI - YYMM - NNNNNN - C
```

| Segment | Meaning |
|---|---|
| `ORI` | fixed product code |
| `YYMM` | 2-digit year + 2-digit month the unit was provisioned |
| `NNNNNN` | 6-digit sequence number, zero-padded, scoped to that `YYMM` (each month gets its own 0-999999 range — a busy month doesn't eat into a future month's headroom) |
| `C` | one Luhn (mod 10) check digit over the preceding digits |

Example: `ORI-2607-000123-4`.

The check digit is the only non-obvious piece: it lets a human — support
staff on a call, someone filling out an RMA form — catch a single mis-typed
or transposed digit by eye/calculator, without a live lookup. It is not
cryptographic and doesn't need to be; its only job is typo detection.

`YYMM` intentionally duplicates part of what `manufacture_date` already says
in full. That's fine — the two fields serve different audiences: the full
date is for firmware/BLE consumers; the embedded `YYMM` is for a human
glancing at a printed label or a serial typed into a support form, without
needing to also have the manufacture-date field in front of them.

## 3. Writing it at manufacturing time

**No custom firmware protocol.** The write path is the same one ESP-IDF
already provides for exactly this use case — a pre-built NVS partition image
flashed directly, no runtime BLE/serial command needed:

```
generate a 2-row NVS CSV (sn, mfg)
        │
        ▼
nvs_partition_gen.py generate <csv> <bin> 0x4000   ← ESP-IDF's own tool
        │
        ▼
esptool.py write_flash 0xFEC000 <bin>               ← same tool already used
                                                        to flash bootloader/app
```

This means provisioning identity is just one more `write_flash` call
alongside whatever the flashing station already does for the bootloader and
app partitions — same tool (`esptool.py`), same station, same workflow,
nothing Ori-specific to install beyond `nvs_partition_gen.py` (ships with
ESP-IDF, or `pip install esp-idf-nvs-partition-gen` standalone). It can run
before or after the app firmware flash; order doesn't matter since they're
independent partitions.

**Reference implementation:** `tools/factory_provision.py`. It:

- Generates the next serial number for a given manufacture date, tracked in
  a local append-only ledger (`tools/factory_serials.csv` by default) so
  re-running across sessions/batches never collides or reissues a number.
- Builds the two-row NVS CSV and shells out to `nvs_partition_gen.py`.
- Either flashes directly to a unit over USB (`--port COM9`) or generates a
  batch of `.bin` images for an offline flashing station (`--count N
  --out-dir batch017/`).

```
python tools/factory_provision.py --port COM9
python tools/factory_provision.py --count 50 --out-dir batch017/ --note "order #4821"
```

**Keep the ledger file durable** (back it up, commit it somewhere outside
the repo's normal churn, whatever fits the actual production process once
one exists) — it is the only record of which serials have already been
issued.

### Verifying a unit after provisioning

Read char `000E` (Device Settings) back over BLE and confirm `"s"`/`"b"`
match what was just written — the same read path Orion already exercises on
every connect (§4 below). A quick way to do this without a full Orion build:
`tools/mock_orion_ble.py`'s existing BLE test harness, or Orion's own Ori
Info modal once paired.

## 4. Exposure to Orion — no new BLE characteristic

Both fields ride the **existing** Device Settings characteristic (char
`000E`) instead of a dedicated one — deliberately, to avoid growing the GATT
table for two static strings. Full wire detail: `ble-protocol.md` §4/§6.4.

- On a **Read**, Ori's response gains two more keys: `"s"` (serial_number)
  and `"b"` (manufacture_date), sourced from `factory_info::serial_number()`/
  `manufacture_date()`.
- On a **Write**, Ori's parser simply never looks for `"s"`/`"b"` — an
  incoming write that happens to include them is a no-op for those two keys
  (the protocol's existing "unknown keys are silently ignored" rule, §4).
  That gives read-only semantics for free, no extra validation code needed.
- Orion surfaces both in its **Ori Info modal** (`pc-app.md`), alongside a
  third piggybacked field, **live signal bars** (`"r"`) — Ori's own RSSI to
  Orion, sampled fresh on every read. That field exists for a genuinely
  different reason than the identity data (Windows can't read a connected
  peripheral's RSSI locally, so Ori has to report its own reading back) but
  rides the same characteristic for the same reason: no new characteristic
  for one more small piece of device state.

## 5. What this does NOT cover

- **Authenticity.** Nothing here proves a given serial/date pair is genuine
  — same caveat `ota.md` already documents for firmware image integrity vs.
  authenticity. If that's ever needed, it's the same answer: ESP32-S3 Secure
  Boot v2 + Flash Encryption at provisioning time (`ota.md`'s deferred-to-M8
  plan), which would also let the factory partition be encrypted at rest.
  Not needed for a serial number's threat model today.
- **Hardware revision / calibration data.** Out of scope for this pass — the
  factory partition is sized (16 KB) with headroom if that's ever wanted
  later, but nothing beyond `"sn"`/`"mfg"` is defined yet.
