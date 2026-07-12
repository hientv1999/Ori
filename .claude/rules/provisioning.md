# Ori — Mass-Production Identity Provisioning

Every Ori unit gets two facts burned in at manufacturing time: a **serial
number** and a **manufacture date**. Both must survive everything short of a
chip-level reflash — firmware updates, factory resets, years of ordinary use
— since they back warranty/RMA lookups and batch investigations. Firmware
side: `esp32-connectivity`/`firmware-core`. Manufacturing tooling: whoever
runs the flashing station.

---

## 1. Why a separate NVS partition

The main "nvs" partition (`firmware/partitions.csv`) is wiped wholesale by
`nvs::factory_reset()` (`Preferences::clear()`) on every factory reset —
serial number and manufacture date must NOT be in that blast radius.

**Chosen: a physically separate NVS partition**, not just a different
namespace in the main one. A namespace would already survive today's
`factory_reset()` (which only clears the `"ori"` namespace), but a separate
partition is also immune to any *future* mistake — a debug `nvs_flash_erase()`
on the whole partition, a "wipe everything" troubleshooting step, a wrong
namespace string — since erasing a partition requires naming it explicitly.
Costs 16 KB of flash carved from the ~9.8 MB `spiffs` partition (ample
headroom — shrinking `spiffs` is already a known-safe operation, done once
before for the `coredump` partition). Worth it given the cost of getting this
wrong (re-flashing a boxed unit, or permanently losing its identity).

```
# firmware/partitions.csv
factory,   data, nvs,      0xFEC000, 0x4000,   # 16 KB, see below
```

- **Type/subtype**: plain `nvs`, a second instance of the same format. Opened
  via `Preferences::begin(name, readOnly, "factory")` (`firmware/src/factory_info.cpp`).
- **Namespace**: `"factory"`, keys `"sn"` (serial number) and `"mfg"`
  (manufacture date, ISO-8601 `"YYYY-MM-DD"`).
- **Firmware never writes to it.** `factory_info.h` exposes only
  `serial_number()`/`manufacture_date()` read accessors — no setter anywhere
  in firmware, BLE, or USB CDC. The only writer is the one-time manufacturing
  flash below — smallest possible attack/bug surface for permanent data.
- **Survives an OTA update**: USB CDC firmware updates (`ota.md`) only ever
  write the `ota_0`/`ota_1` app-slot partitions; every NVS partition is
  untouched by construction, same as bonds/profile today.

## 2. Serial number format

```
ORI - YYMM - NNNNNN - C
```

| Segment | Meaning |
|---|---|
| `ORI` | fixed product code |
| `YYMM` | 2-digit year + 2-digit month the unit was provisioned |
| `NNNNNN` | 6-digit sequence number, zero-padded, scoped to that `YYMM` (each month gets its own 0-999999 range) |
| `C` | one Luhn (mod 10) check digit over the preceding digits |

Example: `ORI-2607-000123-4`.

The check digit lets a human (support staff, an RMA form) catch a single
mis-typed/transposed digit by eye/calculator without a live lookup — typo
detection only, not cryptographic.

`YYMM` intentionally duplicates part of `manufacture_date` — the full date is
for firmware/BLE consumers, the embedded `YYMM` is for a human glancing at a
printed label without the full date field in front of them.

## 3. Writing it at manufacturing time

**No custom firmware protocol** — a pre-built NVS partition image flashed
directly, via ESP-IDF's own tooling:

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

Just one more `write_flash` call alongside whatever the flashing station
already does — same tool, same station, nothing Ori-specific beyond
`nvs_partition_gen.py` (ships with ESP-IDF, or `pip install
esp-idf-nvs-partition-gen`). Order vs. the app firmware flash doesn't matter
(independent partitions).

**Reference implementation:** `tools/factory_provision.py`. It:

- Generates the next serial number for a manufacture date, tracked in a
  local append-only ledger (`tools/factory_serials.csv`) so re-running never
  collides or reissues a number.
- Builds the two-row NVS CSV and shells out to `nvs_partition_gen.py`.
- Either flashes directly over USB (`--port COM9`) or generates a batch of
  `.bin` images for an offline flashing station (`--count N --out-dir batch017/`).

```
python tools/factory_provision.py --port COM9
python tools/factory_provision.py --count 50 --out-dir batch017/ --note "order #4821"
```

**Keep the ledger file durable** — it's the only record of which serials
have already been issued.

### Verifying a unit after provisioning

Read char `000E` (Device Settings) back over BLE and confirm `"s"`/`"b"`
match what was just written (Orion's own connect-time read path). Quick
tools: `tools/mock_orion_ble.py`'s BLE test harness, or Orion's Ori Info
modal once paired.

## 4. Exposure to Orion — no new BLE characteristic

Both fields ride the **existing** Device Settings characteristic (char
`000E`) rather than a dedicated one, to avoid growing the GATT table for two
static strings. Wire detail: `ble-protocol.md` §4/§6.4.

- On **Read**, Ori's response gains `"s"` (serial_number) and `"b"`
  (manufacture_date), from `factory_info::serial_number()`/`manufacture_date()`.
- On **Write**, Ori's parser never looks for `"s"`/`"b"` — an incoming write
  that includes them is a no-op for those keys (§4's "unknown keys ignored"
  rule), giving read-only semantics for free.
- Orion surfaces both in its **Ori Info modal** (`pc-app.md`), alongside a
  third piggybacked field, **live signal bars** (`"r"`) — Ori's own RSSI to
  Orion, sampled fresh on every read. Different motivation (Windows can't
  read a connected peripheral's RSSI locally, so Ori reports its own back)
  but the same "no new characteristic for one more small field" reasoning.

## 5. What this does NOT cover

- **Authenticity.** Nothing proves a serial/date pair is genuine — same
  caveat as `ota.md`'s firmware-integrity-vs-authenticity note. If ever
  needed: ESP32-S3 Secure Boot v2 + Flash Encryption at provisioning time
  (`ota.md`'s deferred-to-M8 plan), which would also encrypt the factory
  partition at rest. Not needed for this threat model today.
- **Hardware revision / calibration data.** Out of scope for this pass — the
  16 KB factory partition has headroom if wanted later, but nothing beyond
  `"sn"`/`"mfg"` is defined yet.
