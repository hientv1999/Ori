// Ori USB CDC OTA receiver (ota.md).
//
// Framing protocol uses magic 0x4F54 ("OT") to distinguish OTA frames from
// boot log bytes that share the same USB CDC port.
//
// Frame format:
//   [0-1] magic     = 0x4F, 0x54
//   [2]   op
//   [3-5] payload_len (uint24 LE)
//   [6..] payload (CBOR or raw bytes)
//
// This module handles the full OTA sequence:
//   BEGIN → validate → READY → DATA (stream) → END → hash check → VALIDATED → restart
//   or on any error → FAILED / REJECT

#include "ota_receiver.h"

#include <Arduino.h>
#include "ori_log.h"
#include <Update.h>
#include <mbedtls/sha256.h>
#include <string.h>

// ArduinoCBOR for decoding BEGIN payload.
#include <cbor.h>

#include "state_machine.h"
#include "ble/gatt_server.h"
#include "screens/screen_ota_updating.h"

// ── Op codes ───────────────────────────────────────────────────────────────

#define OTA_MAGIC_0  0x4F
#define OTA_MAGIC_1  0x54

#define OTA_OP_BEGIN      0x01
#define OTA_OP_READY      0x02
#define OTA_OP_REJECT     0x03
#define OTA_OP_DATA       0x04
#define OTA_OP_PROGRESS   0x05
#define OTA_OP_END        0x06
#define OTA_OP_VALIDATED  0x07
#define OTA_OP_FAILED     0x08

// Progress notification every ~5% of total size.
#define PROGRESS_INTERVAL_PCT 5

// Firmware version — matches Protocol Version char.
static const char k_fw_version[] = "1.0.0";

// ── State machine ──────────────────────────────────────────────────────────

enum class OtaState : uint8_t {
    Idle,
    AwaitingData,
    Complete,
};

namespace {

OtaState g_ota_state     = OtaState::Idle;
bool     g_ota_active    = false;

// SHA-256 context (streaming).
mbedtls_sha256_context g_sha256_ctx;
uint8_t  g_expected_sha256[32] = {};
uint32_t g_total_size     = 0;
uint32_t g_received_bytes = 0;
uint32_t g_next_progress  = 0; // byte count threshold for next PROGRESS frame

// Frame receive buffer.
// Max payload we ever need to buffer: BEGIN CBOR (small), or DATA chunks.
// DATA frames can be up to 4 KB; serial buffer is Arduino's HardwareSerial.
// We read into a rolling 8-byte header buffer then stream DATA directly.
static uint8_t  g_hdr_buf[8];
static int      g_hdr_pos = 0;
static uint8_t* g_payload_buf   = nullptr;
static uint32_t g_payload_len   = 0;
static uint32_t g_payload_read  = 0;
static uint8_t  g_current_op    = 0;

// OTA progress ring pointer (cached from screen_ota_updating).
// We update it directly via widget_progress_ring::set_value when
// PROGRESS threshold is crossed.
// We don't hold the screen pointer directly — we use the static accessor
// pattern: the screen stores its ring in user_data on the screen object.
// For simplicity in M5, we track progress % and let the poll loop post it.
static uint8_t g_progress_pct = 0;
static bool    g_progress_dirty = false;

// ── Output helpers ─────────────────────────────────────────────────────────

// Write a framed OTA response to USB CDC (Serial).
// op + CBOR payload (or empty for READY/VALIDATED).
static void send_frame(uint8_t op, const uint8_t* payload, uint32_t payload_len) {
    uint8_t hdr[6];
    hdr[0] = OTA_MAGIC_0;
    hdr[1] = OTA_MAGIC_1;
    hdr[2] = op;
    hdr[3] = (uint8_t)(payload_len & 0xFF);
    hdr[4] = (uint8_t)((payload_len >> 8) & 0xFF);
    hdr[5] = (uint8_t)((payload_len >> 16) & 0xFF);
    Serial.write(hdr, 6);
    if (payload && payload_len > 0) {
        Serial.write(payload, payload_len);
    }
    Serial.flush();
}

static void send_empty_response(uint8_t op) {
    // CBOR empty map: 0xA0
    uint8_t empty_map = 0xA0;
    send_frame(op, &empty_map, 1);
}

static void send_reject(const char* reason) {
    CborEncoder enc, map;
    uint8_t buf[64];
    cbor_encoder_init(&enc, buf, sizeof(buf), 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_text_stringz(&map, "reason");
    cbor_encode_text_stringz(&map, reason);
    cbor_encoder_close_container(&enc, &map);
    size_t n = cbor_encoder_get_buffer_size(&enc, buf);
    send_frame(OTA_OP_REJECT, buf, (uint32_t)n);
}

static void send_failed(const char* reason) {
    CborEncoder enc, map;
    uint8_t buf[64];
    cbor_encoder_init(&enc, buf, sizeof(buf), 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_text_stringz(&map, "reason");
    cbor_encode_text_stringz(&map, reason);
    cbor_encoder_close_container(&enc, &map);
    size_t n = cbor_encoder_get_buffer_size(&enc, buf);
    send_frame(OTA_OP_FAILED, buf, (uint32_t)n);
}

static void send_progress(uint32_t bytes_received) {
    CborEncoder enc, map;
    uint8_t buf[32];
    cbor_encoder_init(&enc, buf, sizeof(buf), 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_text_stringz(&map, "bytes_received");
    cbor_encode_uint(&map, bytes_received);
    cbor_encoder_close_container(&enc, &map);
    size_t n = cbor_encoder_get_buffer_size(&enc, buf);
    send_frame(OTA_OP_PROGRESS, buf, (uint32_t)n);
}

// ── BEGIN frame handler ────────────────────────────────────────────────────

static void handle_begin(const uint8_t* payload, uint32_t len) {
    CborParser parser;
    CborValue  root, map_val;
    if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) {
        send_reject("cbor_decode");
        return;
    }
    if (!cbor_value_is_map(&root)) {
        send_reject("not_map");
        return;
    }

    char     recv_version[32] = {};
    uint64_t total_size       = 0;
    uint8_t  sha256[32]       = {};
    bool     got_version      = false;
    bool     got_size         = false;
    bool     got_sha          = false;

    cbor_value_enter_container(&root, &map_val);
    while (!cbor_value_at_end(&map_val)) {
        char key[24] = {};
        size_t key_len = sizeof(key) - 1;
        if (cbor_value_is_text_string(&map_val)) {
            cbor_value_copy_text_string(&map_val, key, &key_len, &map_val);
        } else { cbor_value_advance(&map_val); continue; }
        if (cbor_value_at_end(&map_val)) break;

        if (strcmp(key, "fw_version") == 0 && cbor_value_is_text_string(&map_val)) {
            size_t sz = sizeof(recv_version) - 1;
            cbor_value_copy_text_string(&map_val, recv_version, &sz, nullptr);
            got_version = true;
        } else if (strcmp(key, "total_size") == 0 && cbor_value_is_unsigned_integer(&map_val)) {
            cbor_value_get_uint64(&map_val, &total_size);
            got_size = true;
        } else if (strcmp(key, "sha256") == 0 && cbor_value_is_byte_string(&map_val)) {
            size_t sha_len = 32;
            cbor_value_copy_byte_string(&map_val, sha256, &sha_len, nullptr);
            got_sha = (sha_len == 32);
        }
        if (!cbor_value_at_end(&map_val)) cbor_value_advance(&map_val);
    }

    if (!got_size || !got_sha) {
        send_reject("missing_fields");
        return;
    }

    // Validate reject conditions (ota.md).
    if (state_machine::current_state() == AppState::COUNTDOWN) {
        send_reject("countdown_active");
        return;
    }

    // Check against inactive slot capacity (3 MB OTA slot).
    const uint32_t OTA_SLOT_SIZE = 3 * 1024 * 1024;
    if (total_size > OTA_SLOT_SIZE) {
        send_reject("too_large");
        return;
    }

    if (got_version && strcmp(recv_version, k_fw_version) == 0) {
        send_reject("already_current");
        return;
    }

    // Begin the Arduino OTA update.
    if (!Update.begin((size_t)total_size)) {
        send_reject("update_begin_failed");
        return;
    }

    // Store state.
    g_total_size     = (uint32_t)total_size;
    g_received_bytes = 0;
    g_next_progress  = g_total_size / (100 / PROGRESS_INTERVAL_PCT);
    memcpy(g_expected_sha256, sha256, 32);

    // Start SHA-256 streaming context.
    mbedtls_sha256_init(&g_sha256_ctx);
    mbedtls_sha256_starts(&g_sha256_ctx, 0);

    g_ota_state  = OtaState::AwaitingData;
    g_ota_active = true;

    // Block BLE data writes.
    gatt_server::set_ota_active(true);

    // Transition UI to OTA screen.
    state_machine::on_ota_begin();

    // Respond READY.
    send_empty_response(OTA_OP_READY);
    LOG("[ota] BEGIN accepted: size=%u ver=%s\n",
                   (unsigned)total_size, recv_version);
}

// ── DATA frame handler (streaming) ────────────────────────────────────────

static void handle_data_byte(uint8_t byte) {
    if (g_ota_state != OtaState::AwaitingData) return;

    // Stream directly to Update library.
    Update.write(&byte, 1);
    mbedtls_sha256_update(&g_sha256_ctx, &byte, 1);
    g_received_bytes++;

    if (g_received_bytes >= g_next_progress) {
        uint8_t pct = (uint8_t)((100ULL * g_received_bytes) / g_total_size);
        if (pct > 100) pct = 100;
        send_progress(g_received_bytes);
        g_progress_pct   = pct;
        g_progress_dirty = true;
        g_next_progress  += g_total_size / (100 / PROGRESS_INTERVAL_PCT);
        LOG("[ota] progress: %u%%\n", (unsigned)pct);
    }
}

// ── END frame handler ──────────────────────────────────────────────────────

static void handle_end() {
    if (g_ota_state != OtaState::AwaitingData) return;

    // Final progress.
    g_progress_pct   = 100;
    g_progress_dirty = true;

    // Compute final SHA-256.
    uint8_t computed[32];
    mbedtls_sha256_finish(&g_sha256_ctx, computed);
    mbedtls_sha256_free(&g_sha256_ctx);

    if (memcmp(computed, g_expected_sha256, 32) != 0) {
        LOG("[ota] FAILED: hash_mismatch\n");
        Update.abort();
        gatt_server::set_ota_active(false);
        g_ota_state  = OtaState::Idle;
        g_ota_active = false;
        send_failed("hash_mismatch");
        // Return to runtime.
        state_machine::on_reconnect_end();
        return;
    }

    if (!Update.end(true)) {
        LOG("[ota] FAILED: Update.end error=%u\n", (unsigned)Update.getError());
        gatt_server::set_ota_active(false);
        g_ota_state  = OtaState::Idle;
        g_ota_active = false;
        send_failed("flash_error");
        state_machine::on_reconnect_end();
        return;
    }

    send_empty_response(OTA_OP_VALIDATED);
    LOG("[ota] VALIDATED — restarting\n");
    Serial.flush();
    delay(200);
    ESP.restart();
}

// ── Frame parser state machine ─────────────────────────────────────────────
// States: waiting for magic[0], magic[1], op, len[0..2], payload bytes.

enum class ParseState : uint8_t {
    WaitMagic0,
    WaitMagic1,
    WaitOp,
    WaitLen0,
    WaitLen1,
    WaitLen2,
    ReadPayload,
    ReadDataStream, // special — DATA frames are large; streamed directly
};

ParseState g_parse = ParseState::WaitMagic0;

// Small payload buffer for control frames.
static uint8_t g_ctrl_buf[256];
static uint32_t g_ctrl_pos = 0;

static void parse_byte(uint8_t b) {
    switch (g_parse) {
        case ParseState::WaitMagic0:
            if (b == OTA_MAGIC_0) g_parse = ParseState::WaitMagic1;
            break;

        case ParseState::WaitMagic1:
            if (b == OTA_MAGIC_1) g_parse = ParseState::WaitOp;
            else                  g_parse = ParseState::WaitMagic0;
            break;

        case ParseState::WaitOp:
            g_current_op    = b;
            g_payload_len   = 0;
            g_payload_read  = 0;
            g_parse         = ParseState::WaitLen0;
            break;

        case ParseState::WaitLen0:
            g_payload_len  = b;
            g_parse        = ParseState::WaitLen1;
            break;

        case ParseState::WaitLen1:
            g_payload_len |= ((uint32_t)b << 8);
            g_parse        = ParseState::WaitLen2;
            break;

        case ParseState::WaitLen2:
            g_payload_len |= ((uint32_t)b << 16);
            if (g_payload_len == 0) {
                // Zero-length payload.
                if (g_current_op == OTA_OP_END) handle_end();
                g_parse = ParseState::WaitMagic0;
            } else if (g_current_op == OTA_OP_DATA) {
                // Stream DATA bytes directly to Update.write().
                g_parse = ParseState::ReadDataStream;
            } else {
                // Control frame with payload — buffer it.
                g_ctrl_pos = 0;
                g_parse    = ParseState::ReadPayload;
            }
            break;

        case ParseState::ReadPayload:
            if (g_ctrl_pos < sizeof(g_ctrl_buf)) {
                g_ctrl_buf[g_ctrl_pos++] = b;
            }
            g_payload_read++;
            if (g_payload_read >= g_payload_len) {
                // Dispatch.
                switch (g_current_op) {
                    case OTA_OP_BEGIN: handle_begin(g_ctrl_buf, g_ctrl_pos); break;
                    case OTA_OP_END:   handle_end(); break;
                    default: break;
                }
                g_parse = ParseState::WaitMagic0;
            }
            break;

        case ParseState::ReadDataStream:
            handle_data_byte(b);
            g_payload_read++;
            if (g_payload_read >= g_payload_len) {
                g_parse = ParseState::WaitMagic0;
            }
            break;
    }
}

} // namespace

// ── Public API ─────────────────────────────────────────────────────────────

namespace ota_receiver {

void init() {
    g_ota_state      = OtaState::Idle;
    g_ota_active     = false;
    g_parse          = ParseState::WaitMagic0;
    g_progress_pct   = 0;
    g_progress_dirty = false;
    LOG("[ota] receiver ready\n");
}

void poll() {
    // Read available bytes from USB CDC (Serial).
    // In WaitMagic0 state, only consume bytes that start with the OTA magic (0x4F).
    // Non-magic bytes are left in the buffer for other serial consumers (e.g. the
    // debug screen-change command handler in screen_manager).
    while (Serial.available() > 0) {
        if (g_parse == ParseState::WaitMagic0 &&
            (uint8_t)Serial.peek() != OTA_MAGIC_0) {
            break;
        }
        uint8_t b = (uint8_t)Serial.read();
        parse_byte(b);
    }

    // Update OTA progress ring if dirty.
    if (g_progress_dirty && g_ota_active) {
        g_progress_dirty = false;
        screen_ota_updating::set_progress(g_progress_pct);
    }
}

bool is_active() { return g_ota_active; }

void set_progress(uint8_t pct) {
    if (pct > 100) pct = 100;
    g_progress_pct   = pct;
    g_progress_dirty = false;
    screen_ota_updating::set_progress(pct);
}

const char* firmware_version() { return k_fw_version; }

} // namespace ota_receiver
