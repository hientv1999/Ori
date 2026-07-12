#pragma once

// Read-only accessors for mass-production identity data — serial number and
// manufacture date. Backed by the "factory" NVS partition (partitions.csv),
// a physically separate partition from the main "nvs" one so neither
// nvs::factory_reset() nor an app OTA can ever touch it (partitions.csv's own
// comment on the "factory" line, provisioning.md).
//
// Written exactly once, at manufacturing time, by flashing a pre-built NVS
// partition image (tools/factory_provision.py) — there is deliberately no
// runtime setter anywhere in this firmware (BLE, USB CDC, or otherwise).
// On a dev/bench unit whose "factory" partition was never provisioned, both
// accessors return an empty string; callers treat that the same way the rest
// of the protocol treats "not yet known" (ble-protocol.md's "don't show what
// can't be verified" policy — Orion falls back to its own "Unknown" label).
namespace factory_info {

void init();

// e.g. "ORI-2607-000123-4" (product-YYMM-sequence-checkdigit, provisioning.md).
// "" if unprovisioned.
const char* serial_number();

// ISO-8601 date, e.g. "2026-07-12". "" if unprovisioned.
const char* manufacture_date();

} // namespace factory_info
