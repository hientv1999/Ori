#pragma once

// Ori firmware version — single source of truth.
//
// Semver string reported over BLE in the Protocol Version characteristic
// (ble-protocol.md §9) and stamped into the firmware image via the embedded
// "OriFwVer=" marker (g_ori_fw_marker in ota_receiver.cpp). At OTA time the
// receiver checks this embedded version against the fw_version Orion declares at
// BEGIN and rejects on mismatch (ota.md). Bump this one macro on every release;
// gatt_server (Protocol Version char) and ota_receiver both read it here so the
// two can never drift.

#define ORI_FW_VERSION "1.0.0"
