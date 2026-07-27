#pragma once

// Ori's answer to the Orinari IDENTIFY probe over USB CDC.
//
// WHY ORI SPEAKS THIS AT ALL. Ori is a BLE device; it only presents a serial
// port when plugged in for a firmware update. Orion used to find that port by
// USB VID alone (Espressif, 0x303A) — which matches every ESP32 dev board on
// the machine. A single unrelated board plugged in was accepted on faith and
// would have had Ori firmware written onto it, and two Oris made the update
// refuse outright instead of picking the right one. Neither is acceptable for
// an operation that overwrites the application partition.
//
// So Orion now asks, and requires two answers to match before it writes
// anything: that this is an Ori at all (`device_type`), and that it is the
// specific unit Orion is bonded to (`serial`, compared against what it read
// over BLE). See ../Ori/.claude/rules/ota.md and Orion's
// .claude/rules/pc-app-usb-serial.md.
//
// COEXISTENCE WITH THE OTA FRAMING. Both protocols share the one USB CDC port
// and are told apart by their first byte, which cannot collide:
//
//     0x4F 'O'  → OTA frame      (ota_receiver, magic "OT")
//     0xA5      → Orinari frame  (this module)
//     anything  → debug console  (screen_manager::poll_serial)
//
// ota_receiver::poll() already leaves any non-0x4F byte alone when idle, and
// poll_serial() already leaves any 0x4F byte alone. This module slots between
// them on the same rule, claiming only 0xA5.
//
// The frame format is Origale's `proto.c`, unchanged and deliberately so —
// one wire format across the whole Orinari ecosystem:
//
//     SOF(0xA5) | opcode | len | payload[len] | crc8
//     crc8 = CRC-8/SMBUS over opcode, len, payload (SOF excluded)
//
// IDENTIFY is 0x7F (len 0); IDENTITY is 0xFF (len 12): device_type, the three
// firmware version bytes, and the provisioned serial as a u64 little-endian.
// The serial is the device's unique ID; there is no separate chip identifier.

namespace identify_responder {

// Consume any complete IDENTIFY frame waiting on the port and answer it.
// Cheap and non-blocking: returns immediately unless the next byte is 0xA5.
// Must not run while ota_receiver owns the port — main.cpp orders the calls.
void poll();

} // namespace identify_responder
