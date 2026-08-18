#pragma once

// Read-only accessor for mass-production identity data — the serial number.
// Backed by the "factory" NVS partition (partitions.csv), a physically
// separate partition from the main "nvs" one so neither nvs::factory_reset()
// nor an app OTA can ever touch it (partitions.csv's own comment on the
// "factory" line, provisioning.md).
//
// Written exactly once, at manufacturing time, by flashing a pre-built NVS
// partition image (tools/factory_provision.py) — there is deliberately no
// runtime setter anywhere in this firmware (BLE or otherwise).
// On a dev/bench unit whose "factory" partition was never provisioned, the
// accessor returns an empty string; callers treat that the same way the rest
// of the protocol treats "not yet known" (ble-protocol.md's "don't show what
// can't be verified" policy — Orion falls back to its own "Unknown" label).
//
// The manufacture date is NOT stored here (or anywhere on-device): it is the
// serial's own leading DDMMYY digits (provisioning.md §2), and any consumer
// that needs it — Orion's Ori Info modal is the only one today — derives it
// from the serial it already has rather than being sent a second, redundant
// copy of the same fact.
namespace factory_info {

void init();

// e.g. "260726000001" (DDMMYYNNNNCC, provisioning.md §2). "" if unprovisioned.
const char* serial_number();

} // namespace factory_info
