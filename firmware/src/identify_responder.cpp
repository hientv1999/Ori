#include "identify_responder.h"

#include <Arduino.h>
#include <string.h>

#include "factory_info.h"
#include "fw_version.h"
#include "ori_log.h"

namespace {

constexpr uint8_t SOF          = 0xA5;
constexpr uint8_t OP_IDENTIFY  = 0x7F;
constexpr uint8_t OP_IDENTITY  = 0xFF;
constexpr uint8_t IDENTITY_LEN = 12;

// 0x01 Ori, 0x02 Origale, 0x03 Orimat. Also the last two digits of every
// serial (the "CC" of DDMMYYNNNNCC), which is why it is spelled out here
// rather than derived from anything.
constexpr uint8_t DEVICE_TYPE_ORI = 0x01;

// CRC-8/SMBUS: poly 0x07, init 0x00, no reflection, no xorout. Byte-for-byte
// the same routine as Origale's proto.c — do not "optimise" one without the
// other, they are the same wire contract.
uint8_t crc8_update(uint8_t crc, uint8_t byte) {
    crc ^= byte;
    for (uint8_t bit = 0; bit < 8; bit++) {
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

// The provisioned serial, DDMMYYNNNNCC, parsed from the string in the factory
// NVS partition into the plain number the wire format carries.
//
// Returns 0 for an unprovisioned unit (dev boards, which never ran the
// flashing station's provisioning step) and also for any stored value that
// isn't exactly 12 digits. Rejecting rather than best-effort-parsing matters:
// a partially-parsed serial would be a *plausible* wrong answer, and Orion
// gates a destructive firmware write on this value matching. Better to report
// "I don't have one" and let Orion fall back to its explicit unknown-serial
// path than to hand it a number that looks real.
uint64_t provisioned_serial() {
    const char* s = factory_info::serial_number();
    if (!s) return 0;
    size_t n = strlen(s);
    if (n != 12) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    return v;
}

// ORI_FW_VERSION is the single source of truth ("1.0.0"); the wire carries the
// three components as bytes. Anything unparseable reports 0.0.0 rather than
// guessing — the string is a compile-time constant, so that can only happen if
// someone edits fw_version.h into a non-semver shape, and reporting zeros is a
// visible symptom rather than a silent wrong version.
void fw_triplet(uint8_t& major, uint8_t& minor, uint8_t& patch) {
    unsigned a = 0, b = 0, c = 0;
    if (sscanf(ORI_FW_VERSION, "%u.%u.%u", &a, &b, &c) != 3) { a = b = c = 0; }
    major = (uint8_t)a;
    minor = (uint8_t)b;
    patch = (uint8_t)c;
}

void send_identity() {
    uint8_t payload[IDENTITY_LEN];
    payload[0] = DEVICE_TYPE_ORI;
    fw_triplet(payload[1], payload[2], payload[3]);

    // The provisioned serial IS Ori's unique ID on this wire — there is no
    // separate chip identifier in the frame.
    const uint64_t serial = provisioned_serial();
    for (uint8_t b = 0; b < 8; b++) payload[4 + b] = (uint8_t)(serial >> (8 * b));

    uint8_t frame[3 + IDENTITY_LEN + 1];
    frame[0] = SOF;
    frame[1] = OP_IDENTITY;
    frame[2] = IDENTITY_LEN;
    memcpy(&frame[3], payload, IDENTITY_LEN);

    uint8_t crc = 0;
    crc = crc8_update(crc, OP_IDENTITY);
    crc = crc8_update(crc, IDENTITY_LEN);
    for (uint8_t i = 0; i < IDENTITY_LEN; i++) crc = crc8_update(crc, payload[i]);
    frame[3 + IDENTITY_LEN] = crc;

    Serial.write(frame, sizeof(frame));
    LOG("[identify] answered (serial=%llu)\n", (unsigned long long)serial);
}

} // namespace

namespace identify_responder {

void poll() {
    // Only ever claim a frame that is entirely present. IDENTIFY is 4 bytes and
    // arrives in one USB packet, so there is no partial-frame state machine
    // here — if fewer than 4 bytes are buffered, leave them and come back next
    // loop. That keeps this module stateless, and means a truncated or spurious
    // 0xA5 can never leave the port wedged mid-frame the way a stateful parser
    // could.
    while (Serial.available() >= 4) {
        if ((uint8_t)Serial.peek() != SOF) return;  // not ours — leave it

        uint8_t f[4];
        if (Serial.readBytes(f, 4) != 4) return;

        const bool well_formed =
            f[1] == OP_IDENTIFY && f[2] == 0 &&
            f[3] == crc8_update(crc8_update(0, f[1]), f[2]);

        if (well_formed) {
            send_identity();
        } else {
            // A 0xA5 that isn't a valid IDENTIFY. Dropped silently: this port
            // is shared with a debug console and an OTA stream, so noise is
            // expected and answering it would be worse than ignoring it.
            LOG("[identify] dropped malformed 0xA5 frame\n");
        }
    }
}

} // namespace identify_responder
