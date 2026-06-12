// Ori GATT Server — 15 characteristics, ble-protocol.md v1.0.
//
// Service UUID: 6F726900-0000-4F72-9F00-000000000000
// Each char UUID replaces bytes 4-5 with the offset:
//   0001 Protocol Version    Read
//   0002 Device Status       Read+Notify
//   0003 Time Sync           Write (enc)
//   0004 Profile Info        Write (enc)
//   0005 Profile Photo       Write (enc, chunked)
//   0006 Meeting List        Write (enc, chunked)
//   0007 PTO Entry           Write (enc, chunked)
//   0008 Sync Control        Write+Notify (enc)
//   0009 Factory Reset Cmd   Write (enc)
//   000A Sync Manifest       Write+Notify (enc)
//   000B Keyboard Command    Notify (enc)
//   000C Host Volume State   Read+Write (enc)
//   000D Media Metadata      Write+Notify (enc)
//   000E Media Album Art     Write no-rsp (enc, chunked)
//   000F Presence Status     Read+Write (enc)

#include "ble/gatt_server.h"
#include "ble/ble_manager.h"
#include "ble/chunked_transfer.h"
#include "fw_version.h"

// These functions are defined in ble_manager.cpp and called from the
// GATT write callbacks to post events to the main-task event queue.
// widget_profile_card must be included first for the Presence type.
#include "widgets/widget_profile_card.h"

void ble_post_factory_reset_event();
void ble_post_presence_event(widget_profile_card::Presence p);
void ble_post_album_art_event(uint8_t* buf, size_t len);
void ble_post_photo_event(uint8_t* buf, size_t len);
void ble_post_sync_begin_event();
void ble_post_sync_commit_event();
void ble_post_sync_end_event();
void ble_post_orioning_progress(uint8_t pct);
void ble_post_pto_photo_event(uint8_t* buf, size_t len);

#include <Arduino.h>
#include "ori_log.h"
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include <string.h>
#include <string>

// ArduinoCBOR — tiny CBOR encoder/decoder.
// Header-only style included via lib_deps.
#include <cbor.h>

#include "nvs_store.h"
#include "nvs_sync.h"
#include "state_machine.h"
#include "factory_reset.h"
#include "app_state.h"
#include "screens/screen_setup.h"
#include "widgets/widget_profile_card.h"
#include "screens/screen_media_mode.h"
#include "ota_receiver.h"
#include "lcd_panel.h"

// UUIDs ─────────────────────────────────────────────────────────────────────

#define SVC_UUID  "6F726900-0000-4F72-9F00-000000000000"

// Helper: replace bytes 4-5 of base UUID with offset.
// Base: 6F726900-XXXX-4F72-9F00-000000000000
#define CHAR_UUID(n) \
    "6F726900-" #n "-4F72-9F00-000000000000"

// ANCS service UUID (for advertising / discovery)
#define ANCS_UUID "7905F431-B5CE-4E99-A40F-4B1E122D00D0"

// Device Status bytes ───────────────────────────────────────────────────────
#define DS_SETUP_WAITING_PAIRING       0x00
#define DS_SETUP_BONDED_AWAITING_SYNC  0x01
#define DS_SETUP_SYNCING               0x02
#define DS_SETUP_SYNC_COMPLETE         0x03
#define DS_RUNTIME_READY               0x10
#define DS_RUNTIME_RECONNECTING        0x11
#define DS_RUNTIME_SYNCING             0x12
#define DS_ERROR_GENERIC               0xF0

// Firmware version (for Protocol Version characteristic + OTA checks).
// Single source of truth in include/fw_version.h — shared with ota_receiver.
#define FIRMWARE_VERSION ORI_FW_VERSION
#define PROTO_MAJOR 1
#define PROTO_MINOR 1

namespace {

// ── OTA guard ────────────────────────────────────────────────────────────
volatile bool g_ota_active = false;

// ── Device Status ─────────────────────────────────────────────────────────
uint8_t g_device_status = DS_SETUP_WAITING_PAIRING;

// ── Characteristic handles ────────────────────────────────────────────────
NimBLECharacteristic* c_proto_ver   = nullptr; // 0001
NimBLECharacteristic* c_dev_status  = nullptr; // 0002
NimBLECharacteristic* c_time_sync   = nullptr; // 0003
NimBLECharacteristic* c_profile     = nullptr; // 0004
NimBLECharacteristic* c_photo       = nullptr; // 0005
NimBLECharacteristic* c_meetings    = nullptr; // 0006
NimBLECharacteristic* c_pto         = nullptr; // 0007
NimBLECharacteristic* c_sync_ctrl   = nullptr; // 0008
NimBLECharacteristic* c_factory_rst = nullptr; // 0009
NimBLECharacteristic* c_manifest    = nullptr; // 000A
NimBLECharacteristic* c_kbd_cmd     = nullptr; // 000B
NimBLECharacteristic* c_host_vol    = nullptr; // 000C
NimBLECharacteristic* c_media_meta  = nullptr; // 000D
NimBLECharacteristic* c_album_art   = nullptr; // 000E
NimBLECharacteristic* c_presence    = nullptr; // 000F

// ── Chunked transfer contexts ─────────────────────────────────────────────
chunked_transfer::Context g_photo_ctx;
chunked_transfer::Context g_meetings_ctx;
chunked_transfer::Context g_pto_ctx;
chunked_transfer::Context g_art_ctx;

// ── Sync sequence tracking ────────────────────────────────────────────────
uint32_t g_sync_seq = 0;
bool     g_sync_in_progress = false;

// ── Presence state ────────────────────────────────────────────────────────
uint8_t g_presence_byte = 0x03; // OFFLINE default

// ── Volume state ─────────────────────────────────────────────────────────
uint8_t g_volume_level = 50;
bool    g_muted = false;

// ── Vertical-swipe override (drag-wins, ble-protocol.md §12) ─────────────
volatile bool g_vol_swipe_active  = false;
uint32_t      g_vol_swipe_end_ms  = 0;

// ── Current screen pointer (for setup passkey modal) ─────────────────────
// Shared from the screen manager; we look it up via state_machine.
// We don't cache it — screen_setup provides show/hide APIs.

// ─────────────────────────────────────────────────────────────────────────
// CBOR helpers
// ─────────────────────────────────────────────────────────────────────────

// Encode a CBOR map with one text key and uint value.
// Writes into buf, returns byte count written (0 on failure).
static size_t cbor_encode_simple(uint8_t* buf, size_t buf_sz,
                                  const char* key, uint64_t value) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_text_stringz(&map, key);
    cbor_encode_uint(&map, value);
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// Encode {} empty map.
static size_t cbor_encode_empty_map(uint8_t* buf, size_t buf_sz) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 0);
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// Encode SyncControl { op, seq, reason (optional) }.
static size_t cbor_encode_sync_ctrl(uint8_t* buf, size_t buf_sz,
                                     const char* op, uint32_t seq,
                                     const char* reason = nullptr) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    size_t entries = reason ? 3u : 2u;
    cbor_encoder_create_map(&enc, &map, entries);
    cbor_encode_text_stringz(&map, "op");
    cbor_encode_text_stringz(&map, op);
    cbor_encode_text_stringz(&map, "seq");
    cbor_encode_uint(&map, seq);
    if (reason) {
        cbor_encode_text_stringz(&map, "reason");
        cbor_encode_text_stringz(&map, reason);
    }
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// Encode Protocol Version { proto_major, proto_minor, fw_version }.
static size_t cbor_encode_proto_ver(uint8_t* buf, size_t buf_sz) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 3);
    cbor_encode_text_stringz(&map, "proto_major");
    cbor_encode_uint(&map, PROTO_MAJOR);
    cbor_encode_text_stringz(&map, "proto_minor");
    cbor_encode_uint(&map, PROTO_MINOR);
    cbor_encode_text_stringz(&map, "fw_version");
    cbor_encode_text_stringz(&map, FIRMWARE_VERSION);
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// Encode Sync Manifest Notify { needed: [text,...] }.
static size_t cbor_encode_manifest_notify(uint8_t* buf, size_t buf_sz,
                                           const char** needed, size_t n) {
    CborEncoder enc, map, arr;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_text_stringz(&map, "needed");
    cbor_encoder_create_array(&map, &arr, n);
    for (size_t i = 0; i < n; ++i) {
        cbor_encode_text_stringz(&arr, needed[i]);
    }
    cbor_encoder_close_container(&map, &arr);
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// ── SHA-256 of a CBOR blob (deterministic, for hash-manifest delta) ────────

static void sha256_of_buf(const uint8_t* buf, size_t len, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, /*is224=*/0);
    mbedtls_sha256_update(&ctx, buf, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

// ─────────────────────────────────────────────────────────────────────────
// Sync staging (PSRAM) — ble-protocol.md §6.0
//
// Everything written between SyncControl{BEGIN} and {END} is held here
// (small fields inline, blob items in PSRAM) instead of touching NVS or
// live UI state. stage_commit() applies it all in one burst at END,
// mirroring the OTA stage-then-flash pattern.
// ─────────────────────────────────────────────────────────────────────────

struct SyncStage {
    bool     active         = false;
    uint32_t total_bytes    = 0;
    uint32_t received_bytes = 0;
    uint8_t  last_pct_sent  = 0xFF; // sentinel so 0% is posted at least once

    bool     have_time = false;
    uint64_t epoch_utc = 0;
    char     tz[64]    = {};

    bool     have_profile = false;
    uint8_t* profile_cbor = nullptr; // owned
    size_t   profile_len  = 0;

    bool     have_photo = false;
    uint8_t* photo_jpeg = nullptr;   // owned (PSRAM, from chunked_transfer)
    size_t   photo_len  = 0;

    bool     have_meetings = false;
    uint8_t* meetings_cbor = nullptr; // owned (PSRAM, from chunked_transfer)
    size_t   meetings_len  = 0;

    bool     have_pto = false;
    uint8_t* pto_cbor = nullptr;     // owned (PSRAM, from chunked_transfer)
    size_t   pto_len  = 0;
};

SyncStage g_stage;

// Forward declarations — apply_* helpers are defined just below stage_commit
// uses them, but stage_reset() needs to free their buffers first.
static void apply_time_sync(uint64_t epoch_utc, const char* tz);
static void apply_profile_cbor(const uint8_t* data, size_t len);
static void apply_photo_jpeg(uint8_t* buf, size_t n);
static void apply_meetings_cbor(uint8_t* buf, size_t n);
static void apply_pto_cbor(uint8_t* buf, size_t n);

// Free any owned staging buffers and zero the struct.
static void stage_reset() {
    if (g_stage.profile_cbor)  heap_caps_free(g_stage.profile_cbor);
    if (g_stage.photo_jpeg)    heap_caps_free(g_stage.photo_jpeg);
    if (g_stage.meetings_cbor) heap_caps_free(g_stage.meetings_cbor);
    if (g_stage.pto_cbor)      heap_caps_free(g_stage.pto_cbor);
    g_stage = SyncStage{};
}

// Begin a new staging session (called on SyncControl{BEGIN}).
static void stage_begin(uint32_t total_bytes) {
    stage_reset();
    g_stage.active      = true;
    g_stage.total_bytes = total_bytes;
}

// Account for `n` newly received application-payload bytes and, while the
// setup orioning modal is on screen, post byte-accurate progress (capped at
// 99% — 100% is posted only after stage_commit() finishes at END). Gating on
// DS_SETUP_SYNCING avoids posting to update_orioning_progress() when the
// active screen's user_data isn't a SetupState* (reconnect / media mode).
static void stage_add_bytes(uint16_t n) {
    if (!g_stage.active) return;
    g_stage.received_bytes += n;
    if (g_stage.total_bytes == 0) return;
    if (g_device_status != DS_SETUP_SYNCING) return;

    uint32_t pct = (uint32_t)((uint64_t)g_stage.received_bytes * 100u / g_stage.total_bytes);
    if (pct > 99) pct = 99;
    if ((uint8_t)pct != g_stage.last_pct_sent) {
        g_stage.last_pct_sent = (uint8_t)pct;
        ble_post_orioning_progress((uint8_t)pct);
    }
}

// Apply every staged item to NVS / live state in one burst, then reset.
// Called on SyncControl{END}.
static void stage_commit() {
    if (g_stage.have_time) {
        apply_time_sync(g_stage.epoch_utc, g_stage.tz);
    }
    if (g_stage.have_profile) {
        apply_profile_cbor(g_stage.profile_cbor, g_stage.profile_len);
    }
    if (g_stage.have_photo) {
        apply_photo_jpeg(g_stage.photo_jpeg, g_stage.photo_len);
        g_stage.photo_jpeg = nullptr; // ownership transferred
    }
    if (g_stage.have_meetings) {
        apply_meetings_cbor(g_stage.meetings_cbor, g_stage.meetings_len);
        g_stage.meetings_cbor = nullptr; // freed by apply_meetings_cbor
    }
    if (g_stage.have_pto) {
        apply_pto_cbor(g_stage.pto_cbor, g_stage.pto_len);
        g_stage.pto_cbor = nullptr; // freed by apply_pto_cbor
    }

    if (g_stage.total_bytes > 0 && g_device_status == DS_SETUP_SYNCING) {
        ble_post_orioning_progress(100);
    }

    stage_reset();
}

// ─────────────────────────────────────────────────────────────────────────
// Characteristic write callbacks (NimBLE calls these from its own task)
// ─────────────────────────────────────────────────────────────────────────

// Guard: return false (NACK) if OTA is active or link is not encrypted.
static bool check_write_allowed(NimBLEConnInfo& info, const char* char_name) {
    if (g_ota_active) {
        LOG("[gatt] NACK %s: OTA active\n", char_name);
        return false;
    }
    if (!info.isEncrypted()) {
        LOG("[gatt] NACK %s: not encrypted\n", char_name);
        return false;
    }
    return true;
}

// ── Characteristic callbacks (one class per char, or a shared dispatcher) ─

class OriCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        const std::string& val = c->getValue();
        const uint8_t* data    = reinterpret_cast<const uint8_t*>(val.data());
        uint16_t        len    = (uint16_t)val.size();

        if (c == c_time_sync) {
            handle_time_sync(data, len, info);
        } else if (c == c_profile) {
            handle_profile(data, len, info);
        } else if (c == c_photo) {
            handle_photo(data, len, info);
        } else if (c == c_meetings) {
            handle_meetings(data, len, info);
        } else if (c == c_pto) {
            handle_pto(data, len, info);
        } else if (c == c_sync_ctrl) {
            handle_sync_ctrl(data, len, info);
        } else if (c == c_factory_rst) {
            handle_factory_reset(data, len, info);
        } else if (c == c_manifest) {
            handle_manifest_write(data, len, info);
        } else if (c == c_host_vol) {
            handle_host_volume(data, len, info);
        } else if (c == c_media_meta) {
            handle_media_metadata(data, len, info);
        } else if (c == c_album_art) {
            handle_album_art(data, len, info);
        } else if (c == c_presence) {
            handle_presence(data, len, info);
        }
    }

    void onRead(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        if (c == c_proto_ver) {
            uint8_t buf[64];
            size_t n = cbor_encode_proto_ver(buf, sizeof(buf));
            c->setValue(buf, n);
        } else if (c == c_dev_status) {
            c->setValue(&g_device_status, 1);
        } else if (c == c_host_vol) {
            if (!info.isEncrypted()) return;
            handle_host_volume_read(c);
        } else if (c == c_presence) {
            if (!info.isEncrypted()) return;
            c->setValue(&g_presence_byte, 1);
        }
    }

private:

    // ── Time Sync (char 0003) ───────────────────────────────────────────────
    // Staged — applied at SyncControl{END} via apply_time_sync() (§6.0).
    void handle_time_sync(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "TimeSync")) return;
        stage_add_bytes(len);

        CborParser parser;
        CborValue  root, map_val;
        if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) {
            LOG("[gatt] TimeSync: CBOR parse error\n");
            return;
        }
        if (!cbor_value_is_map(&root)) return;

        uint64_t epoch_utc = 0;
        char     tz[64]    = {};

        cbor_value_enter_container(&root, &map_val);
        while (!cbor_value_at_end(&map_val)) {
            char key[32] = {};
            size_t key_len = sizeof(key) - 1;
            if (cbor_value_is_text_string(&map_val)) {
                cbor_value_copy_text_string(&map_val, key, &key_len, &map_val);
            } else {
                cbor_value_advance(&map_val);
            }

            if (strcmp(key, "epoch_utc") == 0 && cbor_value_is_unsigned_integer(&map_val)) {
                cbor_value_get_uint64(&map_val, &epoch_utc);
            } else if (strcmp(key, "tz") == 0 && cbor_value_is_text_string(&map_val)) {
                size_t tz_len = sizeof(tz) - 1;
                cbor_value_copy_text_string(&map_val, tz, &tz_len, nullptr);
            }
            if (!cbor_value_at_end(&map_val)) cbor_value_advance(&map_val);
        }

        if (epoch_utc > 0) {
            g_stage.have_time = true;
            g_stage.epoch_utc = epoch_utc;
            strncpy(g_stage.tz, tz, sizeof(g_stage.tz) - 1);
        }
    }

    // ── Profile Info (char 0004) ────────────────────────────────────────────
    // Staged — parsed and applied at SyncControl{END} via apply_profile_cbor() (§6.0).
    void handle_profile(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "ProfileInfo")) return;

        if (g_stage.profile_cbor) {
            heap_caps_free(g_stage.profile_cbor);
            g_stage.profile_cbor = nullptr;
        }
        g_stage.profile_cbor = static_cast<uint8_t*>(
            heap_caps_malloc(len > 0 ? len : 1, MALLOC_CAP_8BIT));
        if (g_stage.profile_cbor) {
            memcpy(g_stage.profile_cbor, data, len);
            g_stage.profile_len  = len;
            g_stage.have_profile = true;
        }

        stage_add_bytes(len);
    }

    // ── Profile Photo (char 0005) — chunked ────────────────────────────────
    // Staged — hashed and posted at SyncControl{END} via apply_photo_jpeg() (§6.0).
    void handle_photo(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "ProfilePhoto")) return;

        if (!g_photo_ctx.on_complete) {
            g_photo_ctx.on_complete = [](uint8_t* buf, size_t n, const char* nack) {
                if (nack) {
                    LOG("[gatt] Photo NACK: %s\n", nack);
                    return;
                }
                g_stage.have_photo = true;
                g_stage.photo_jpeg = buf; // ownership moves to SyncStage
                g_stage.photo_len  = n;
            };
            g_photo_ctx.on_fragment = [](uint16_t seq, uint16_t total, uint16_t plen) {
                stage_add_bytes(plen);
            };
        }
        chunked_transfer::feed(&g_photo_ctx, data, len);
    }

    // ── Meeting List (char 0006) — chunked ─────────────────────────────────
    // Staged — hashed and applied at SyncControl{END} via apply_meetings_cbor() (§6.0).
    void handle_meetings(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "MeetingList")) return;

        if (!g_meetings_ctx.on_complete) {
            g_meetings_ctx.on_complete = [](uint8_t* buf, size_t n, const char* nack) {
                if (nack) {
                    LOG("[gatt] Meetings NACK: %s\n", nack);
                    return;
                }
                g_stage.have_meetings  = true;
                g_stage.meetings_cbor  = buf; // ownership moves to SyncStage
                g_stage.meetings_len   = n;
            };
            g_meetings_ctx.on_fragment = [](uint16_t seq, uint16_t total, uint16_t plen) {
                stage_add_bytes(plen);
            };
        }
        chunked_transfer::feed(&g_meetings_ctx, data, len);
    }

    // ── PTO Entry (char 0007) — chunked ────────────────────────────────────
    // Staged — hashed, parsed, and applied at SyncControl{END} via apply_pto_cbor() (§6.0).
    void handle_pto(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "PtoEntry")) return;

        if (!g_pto_ctx.on_complete) {
            g_pto_ctx.on_complete = [](uint8_t* buf, size_t n, const char* nack) {
                if (nack) {
                    LOG("[gatt] PTO NACK: %s\n", nack);
                    return;
                }
                g_stage.have_pto = true;
                g_stage.pto_cbor = buf; // ownership moves to SyncStage
                g_stage.pto_len  = n;
            };
            g_pto_ctx.on_fragment = [](uint16_t seq, uint16_t total, uint16_t plen) {
                stage_add_bytes(plen);
            };
        }
        chunked_transfer::feed(&g_pto_ctx, data, len);
    }

    // ── Sync Control (char 0008) ────────────────────────────────────────────
    void handle_sync_ctrl(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "SyncControl")) return;

        CborParser parser;
        CborValue  root, map_val;
        if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) return;
        if (!cbor_value_is_map(&root)) return;

        char     op[8]  = {};
        uint64_t seq    = 0;
        uint64_t total  = 0;

        cbor_value_enter_container(&root, &map_val);
        while (!cbor_value_at_end(&map_val)) {
            char key[16] = {};
            size_t key_len = sizeof(key) - 1;
            if (cbor_value_is_text_string(&map_val)) {
                cbor_value_copy_text_string(&map_val, key, &key_len, &map_val);
            } else { cbor_value_advance(&map_val); continue; }
            if (cbor_value_at_end(&map_val)) break;

            if (strcmp(key, "op") == 0 && cbor_value_is_text_string(&map_val)) {
                size_t sz = sizeof(op) - 1;
                cbor_value_copy_text_string(&map_val, op, &sz, nullptr);
            } else if (strcmp(key, "seq") == 0 && cbor_value_is_unsigned_integer(&map_val)) {
                cbor_value_get_uint64(&map_val, &seq);
            } else if (strcmp(key, "total") == 0 && cbor_value_is_unsigned_integer(&map_val)) {
                cbor_value_get_uint64(&map_val, &total);
            }
            if (!cbor_value_at_end(&map_val)) cbor_value_advance(&map_val);
        }

        LOG("[gatt] SyncControl op=%s seq=%u total=%u\n", op, (unsigned)seq, (unsigned)total);

        if (strcmp(op, "BEGIN") == 0) {
            // Handshake: a valid SyncControl{BEGIN} on the encrypted link proves
            // the bonded peer is the Orion app. If it's the provisional Orion
            // (Step 2), this commits the bond; otherwise it's a no-op.
            ble_manager::confirm_orion_peer();
            g_sync_seq         = (uint32_t)seq;
            g_sync_in_progress = true;
            stage_begin((uint32_t)total);
            uint8_t new_status = (g_device_status <= DS_SETUP_SYNC_COMPLETE)
                                  ? DS_SETUP_SYNCING
                                  : DS_RUNTIME_SYNCING;
            gatt_server::set_device_status(new_status);
            // Defer orioning modal to main task (LVGL must not be called from NimBLE task).
            ble_post_sync_begin_event();

        } else if (strcmp(op, "END") == 0) {
            g_sync_in_progress = false;
            // stage_commit() does multiple NVS flash writes + LVGL profile-card
            // updates — defer the whole burst to the main task (gatt_server::
            // run_staged_commit(), called from ble_manager::poll() before
            // lv_timer_handler()) so it can't collide with LCD_CAM DMA on the
            // NimBLE host task and trip the interrupt watchdog (§6.0).
            ble_post_sync_commit_event();
        }
    }

    // ── Factory Reset Command (char 0009) ───────────────────────────────────
    void handle_factory_reset(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "FactoryReset")) return;
        if (len < 4) return;

        // Magic: 0xFA 0xC7 0x5E 0x5E
        if (data[0] == 0xFA && data[1] == 0xC7 &&
            data[2] == 0x5E && data[3] == 0x5E) {
            LOG("[gatt] Remote factory reset triggered\n");
            // IMPORTANT: do not call factory_reset::execute() directly from this
            // BLE callback — deferred to main loop via ble_manager event queue.
            ble_manager::wipe_all_bonds(); // wipe bonds immediately
            // Post event to main loop for NVS wipe + restart.
            ble_post_factory_reset_event();
        } else {
            LOG("[gatt] FactoryReset: bad magic\n");
            // Send NACK back to Orion via SyncControl.
            uint8_t buf[64];
            size_t  n = cbor_encode_sync_ctrl(buf, sizeof(buf),
                                               "NACK", g_sync_seq, "NACK_BAD_MAGIC");
            if (c_sync_ctrl) c_sync_ctrl->notify(buf, n);
        }
    }

    // ── Sync Manifest Write (char 000A) — central→peripheral ───────────────
    void handle_manifest_write(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "SyncManifest")) return;

        CborParser parser;
        CborValue  root, map_val;
        if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) return;
        if (!cbor_value_is_map(&root)) return;

        // Received hashes from Orion.
        uint8_t recv_profile[32]  = {};
        uint8_t recv_photo[32]    = {};
        uint8_t recv_meetings[32] = {};
        uint8_t recv_pto[32]      = {};
        bool    got_profile  = false, got_photo = false;
        bool    got_meetings = false, got_pto   = false;

        cbor_value_enter_container(&root, &map_val);
        while (!cbor_value_at_end(&map_val)) {
            char key[20] = {};
            size_t key_len = sizeof(key) - 1;
            if (cbor_value_is_text_string(&map_val)) {
                cbor_value_copy_text_string(&map_val, key, &key_len, &map_val);
            } else { cbor_value_advance(&map_val); continue; }
            if (cbor_value_at_end(&map_val)) break;

            if (cbor_value_is_byte_string(&map_val)) {
                size_t sz = 32;
                if (strcmp(key, "profile_sha") == 0) {
                    cbor_value_copy_byte_string(&map_val, recv_profile, &sz, nullptr);
                    got_profile = (sz == 32);
                } else if (strcmp(key, "photo_sha") == 0) {
                    cbor_value_copy_byte_string(&map_val, recv_photo, &sz, nullptr);
                    got_photo = (sz == 32);
                } else if (strcmp(key, "meetings_sha") == 0) {
                    cbor_value_copy_byte_string(&map_val, recv_meetings, &sz, nullptr);
                    got_meetings = (sz == 32);
                } else if (strcmp(key, "pto_sha") == 0) {
                    cbor_value_copy_byte_string(&map_val, recv_pto, &sz, nullptr);
                    got_pto = (sz == 32);
                }
            }
            if (!cbor_value_at_end(&map_val)) cbor_value_advance(&map_val);
        }

        // Compare against stored hashes.
        uint8_t stored[32];
        const char* needed[4];
        size_t needed_count = 0;

        if (got_profile) {
            bool match = nvs_sync::load_hash(nvs_sync::HASH_KEY_PROFILE, stored)
                         && memcmp(stored, recv_profile, 32) == 0;
            if (!match) needed[needed_count++] = "profile";
        }
        if (got_photo) {
            bool match = nvs_sync::load_hash(nvs_sync::HASH_KEY_PHOTO, stored)
                         && memcmp(stored, recv_photo, 32) == 0;
            if (!match) needed[needed_count++] = "photo";
        }
        if (got_meetings) {
            bool match = nvs_sync::load_hash(nvs_sync::HASH_KEY_MEETINGS, stored)
                         && memcmp(stored, recv_meetings, 32) == 0;
            if (!match) needed[needed_count++] = "meetings";
        }
        if (got_pto) {
            bool match = nvs_sync::load_hash(nvs_sync::HASH_KEY_PTO, stored)
                         && memcmp(stored, recv_pto, 32) == 0;
            if (!match) needed[needed_count++] = "pto";
        }

        // Notify Orion what we need.
        uint8_t buf[256];
        size_t  n = cbor_encode_manifest_notify(buf, sizeof(buf),
                                                  needed, needed_count);
        if (c_manifest) c_manifest->notify(buf, n);

        LOG("[gatt] Manifest: need %u items\n", (unsigned)needed_count);
    }

    // ── Host Volume State (char 000C) ───────────────────────────────────────
    void handle_host_volume(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "HostVolume")) return;

        // Drag-wins override: ignore incoming push during/after swipe.
        uint32_t now = (uint32_t)millis();
        if (g_vol_swipe_active || (now - g_vol_swipe_end_ms < 800)) {
            LOG("[gatt] HostVolume: ignoring during swipe override\n");
            return;
        }

        CborParser parser;
        CborValue  root, map_val;
        if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) return;
        if (!cbor_value_is_map(&root)) return;

        uint64_t level = g_volume_level;
        bool     mute  = g_muted;

        cbor_value_enter_container(&root, &map_val);
        while (!cbor_value_at_end(&map_val)) {
            char key[16] = {};
            size_t key_len = sizeof(key) - 1;
            if (cbor_value_is_text_string(&map_val)) {
                cbor_value_copy_text_string(&map_val, key, &key_len, &map_val);
            } else { cbor_value_advance(&map_val); continue; }
            if (cbor_value_at_end(&map_val)) break;

            if (strcmp(key, "level") == 0 && cbor_value_is_unsigned_integer(&map_val)) {
                cbor_value_get_uint64(&map_val, &level);
            } else if (strcmp(key, "mute") == 0 && cbor_value_is_boolean(&map_val)) {
                cbor_value_get_boolean(&map_val, &mute);
            }
            if (!cbor_value_at_end(&map_val)) cbor_value_advance(&map_val);
        }

        g_volume_level = (uint8_t)(level > 100 ? 100 : level);
        g_muted        = mute;

        // Update app_state so the media mode screen reflects it.
        app_state::set_media_volume((int)g_volume_level);
        LOG("[gatt] HostVolume: level=%u mute=%d\n",
                       (unsigned)g_volume_level, (int)g_muted);
    }

    void handle_host_volume_read(NimBLECharacteristic* c) {
        // Encode { level, mute } and set the characteristic value.
        CborEncoder enc, map;
        uint8_t buf[32];
        cbor_encoder_init(&enc, buf, sizeof(buf), 0);
        cbor_encoder_create_map(&enc, &map, 2);
        cbor_encode_text_stringz(&map, "level");
        cbor_encode_uint(&map, g_volume_level);
        cbor_encode_text_stringz(&map, "mute");
        cbor_encode_boolean(&map, g_muted);
        cbor_encoder_close_container(&enc, &map);
        size_t n = cbor_encoder_get_buffer_size(&enc, buf);
        c->setValue(buf, n);
    }

    // ── Media Metadata (char 000D) ──────────────────────────────────────────
    void handle_media_metadata(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "MediaMetadata")) return;

        CborParser parser;
        CborValue  root, map_val;
        if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) return;
        if (!cbor_value_is_map(&root)) return;

        char  title[193]  = {};
        char  artist[97]  = {};
        bool  can_seek    = false;

        cbor_value_enter_container(&root, &map_val);
        while (!cbor_value_at_end(&map_val)) {
            char key[16] = {};
            size_t key_len = sizeof(key) - 1;
            if (cbor_value_is_text_string(&map_val)) {
                cbor_value_copy_text_string(&map_val, key, &key_len, &map_val);
            } else { cbor_value_advance(&map_val); continue; }
            if (cbor_value_at_end(&map_val)) break;

            if (strcmp(key, "title") == 0 && cbor_value_is_text_string(&map_val)) {
                size_t sz = sizeof(title) - 1;
                cbor_value_copy_text_string(&map_val, title, &sz, nullptr);
            } else if (strcmp(key, "artist") == 0 && cbor_value_is_text_string(&map_val)) {
                size_t sz = sizeof(artist) - 1;
                cbor_value_copy_text_string(&map_val, artist, &sz, nullptr);
            } else if (strcmp(key, "can_seek") == 0 && cbor_value_is_boolean(&map_val)) {
                cbor_value_get_boolean(&map_val, &can_seek);
            }
            if (!cbor_value_at_end(&map_val)) cbor_value_advance(&map_val);
        }

        LOG("[gatt] MediaMetadata: title='%s' artist='%s' can_seek=%d\n",
                       title, artist, (int)can_seek);
        app_state::set_media_meta(title, artist, can_seek);
    }

    // ── Media Album Art (char 000E) — chunked raw JPEG ─────────────────────
    void handle_album_art(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "AlbumArt")) return;

        if (!g_art_ctx.on_complete) {
            g_art_ctx.on_complete = [](uint8_t* buf, size_t n, const char* nack) {
                if (nack) {
                    LOG("[gatt] AlbumArt NACK: %s\n", nack);
                    return;
                }
                // JPEG decoded via LVGL's TJPGD decoder.
                // The buf is already in PSRAM. Store in g_album_art_psram and
                // signal the media mode screen to reload.
                ble_post_album_art_event(buf, n);
            };
        }
        chunked_transfer::feed(&g_art_ctx, data, len);
    }

    // ── Presence Status (char 000F) — single byte ──────────────────────────
    void handle_presence(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "PresenceStatus")) return;
        if (len < 1) return;

        uint8_t presence = data[0];
        if (presence > 0x03) {
            LOG("[gatt] PresenceStatus: invalid value 0x%02X\n", (unsigned)presence);
            // NACK via SyncControl
            uint8_t buf[64];
            size_t  n = cbor_encode_sync_ctrl(buf, sizeof(buf),
                                               "NACK", 0, "NACK_CBOR_DECODE");
            if (c_sync_ctrl) c_sync_ctrl->notify(buf, n);
            return;
        }

        g_presence_byte = presence;
        LOG("[gatt] PresenceStatus: 0x%02X\n", (unsigned)presence);

        // Map byte to widget_profile_card::Presence and update the UI.
        widget_profile_card::Presence p;
        switch (presence) {
            case 0x00: p = widget_profile_card::Presence::Available; break;
            case 0x01: p = widget_profile_card::Presence::Busy;      break;
            case 0x02: p = widget_profile_card::Presence::Away;      break;
            default:   p = widget_profile_card::Presence::Offline;   break;
        }

        // Update the default presence so new screens pick it up.
        widget_profile_card::set_default_presence(p);

        // Post event to main-task queue so the profile card widget
        // update runs safely on the LVGL task.
        ble_post_presence_event(p);
    }
};

static OriCharacteristicCallbacks s_char_cb;

// ─────────────────────────────────────────────────────────────────────────
// Sync stage commit helpers — apply one staged item to NVS / live state.
// Called only from stage_commit() at SyncControl{END} (§6.0).
// ─────────────────────────────────────────────────────────────────────────

static void apply_time_sync(uint64_t epoch_utc, const char* tz) {
    if (epoch_utc == 0) return;

    struct timeval tv = { .tv_sec = (time_t)epoch_utc, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    if (tz && tz[0]) {
        setenv("TZ", tz, 1);
        tzset();
        nvs_sync::save_tz(tz);
    }

    LOG("[gatt] TimeSync: epoch=%llu tz=%s\n",
                   (unsigned long long)epoch_utc, tz ? tz : "");
}

static void apply_profile_cbor(const uint8_t* data, size_t len) {
    CborParser parser;
    CborValue  root, map_val;
    if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) return;
    if (!cbor_value_is_map(&root)) return;

    char name[65]  = {};
    char title[65] = {};
    char email[129]= {};
    char phone[33] = {};

    cbor_value_enter_container(&root, &map_val);
    while (!cbor_value_at_end(&map_val)) {
        char key[16] = {};
        size_t key_len = sizeof(key) - 1;
        if (cbor_value_is_text_string(&map_val)) {
            cbor_value_copy_text_string(&map_val, key, &key_len, &map_val);
        } else { cbor_value_advance(&map_val); continue; }

        if (cbor_value_at_end(&map_val)) break;

        if (strcmp(key, "name") == 0 && cbor_value_is_text_string(&map_val)) {
            size_t sz = sizeof(name) - 1;
            cbor_value_copy_text_string(&map_val, name, &sz, nullptr);
        } else if (strcmp(key, "title") == 0 && cbor_value_is_text_string(&map_val)) {
            size_t sz = sizeof(title) - 1;
            cbor_value_copy_text_string(&map_val, title, &sz, nullptr);
        } else if (strcmp(key, "email") == 0 && cbor_value_is_text_string(&map_val)) {
            size_t sz = sizeof(email) - 1;
            cbor_value_copy_text_string(&map_val, email, &sz, nullptr);
        } else if (strcmp(key, "phone") == 0 && cbor_value_is_text_string(&map_val)) {
            size_t sz = sizeof(phone) - 1;
            cbor_value_copy_text_string(&map_val, phone, &sz, nullptr);
        }
        if (!cbor_value_at_end(&map_val)) cbor_value_advance(&map_val);
    }

    // Save to NVS.
    nvs_sync::save_profile(name, title, email, phone);

    // Compute SHA-256 of the raw CBOR bytes and store.
    uint8_t hash[32];
    sha256_of_buf(data, len, hash);
    nvs_sync::save_hash(nvs_sync::HASH_KEY_PROFILE, hash);

    // Update the live profile card.
    widget_profile_card::set_profile(name, title, email, phone);

    LOG("[gatt] ProfileInfo: name=%s title=%s\n", name, title);
}

static void apply_photo_jpeg(uint8_t* buf, size_t n) {
    uint8_t hash[32];
    sha256_of_buf(buf, n, hash);
    nvs_sync::save_hash(nvs_sync::HASH_KEY_PHOTO, hash);
    LOG("[gatt] Photo received: %u bytes\n", (unsigned)n);
    ble_post_photo_event(buf, n); // ownership transferred to event handler
}

static void apply_meetings_cbor(uint8_t* buf, size_t n) {
    uint8_t hash[32];
    sha256_of_buf(buf, n, hash);
    nvs_sync::save_hash(nvs_sync::HASH_KEY_MEETINGS, hash);
    LOG("[gatt] Meetings received: %u bytes, hash stored\n", (unsigned)n);
    state_machine::set_meetings_cbor(buf, n, /*save_to_nvs=*/true);
    heap_caps_free(buf);
}

static void apply_pto_cbor(uint8_t* buf, size_t n) {
    // Hash the full CBOR payload for delta-sync manifest.
    uint8_t hash[32];
    sha256_of_buf(buf, n, hash);
    nvs_sync::save_hash(nvs_sync::HASH_KEY_PTO, hash);

    // ── Parse PtoEntry CBOR ────────────────────────────────────
    // Schema: { start:uint, end:uint, destination:text, image:bytes }
    CborParser parser;
    CborValue  root, map_val;
    uint32_t   pto_start = 0, pto_end = 0;
    char       dest[129] = {};
    uint8_t*   img_buf   = nullptr;
    size_t     img_len   = 0;

    if (cbor_parser_init(buf, n, 0, &parser, &root) == CborNoError &&
        cbor_value_is_map(&root)) {

        cbor_value_enter_container(&root, &map_val);
        while (!cbor_value_at_end(&map_val)) {
            char key[16] = {};
            size_t ksz = sizeof(key) - 1;
            if (!cbor_value_is_text_string(&map_val)) {
                cbor_value_advance(&map_val);
                continue;
            }
            cbor_value_copy_text_string(&map_val, key, &ksz, &map_val);
            if (cbor_value_at_end(&map_val)) break;

            if (strcmp(key, "start") == 0 &&
                cbor_value_is_unsigned_integer(&map_val)) {
                uint64_t v = 0;
                cbor_value_get_uint64(&map_val, &v);
                pto_start = (uint32_t)v;

            } else if (strcmp(key, "end") == 0 &&
                       cbor_value_is_unsigned_integer(&map_val)) {
                uint64_t v = 0;
                cbor_value_get_uint64(&map_val, &v);
                pto_end = (uint32_t)v;

            } else if (strcmp(key, "destination") == 0 &&
                       cbor_value_is_text_string(&map_val)) {
                size_t dsz = sizeof(dest) - 1;
                cbor_value_copy_text_string(&map_val, dest, &dsz, nullptr);

            } else if (strcmp(key, "image") == 0 &&
                       cbor_value_is_byte_string(&map_val)) {
                size_t raw_len = 0;
                cbor_value_get_string_length(&map_val, &raw_len);
                if (raw_len > 0 && raw_len <= 512 * 1024) {
                    img_buf = static_cast<uint8_t*>(
                        heap_caps_malloc(raw_len,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    if (!img_buf)
                        img_buf = static_cast<uint8_t*>(malloc(raw_len));
                    if (img_buf) {
                        img_len = raw_len;
                        cbor_value_copy_byte_string(
                            &map_val, img_buf, &img_len, nullptr);
                    }
                }
                // raw_len == 0 → no image set; img_buf stays nullptr
            }

            if (!cbor_value_at_end(&map_val))
                cbor_value_advance(&map_val);
        }
    }

    heap_caps_free(buf); // free staged CBOR blob

    nvs_sync::save_pto_meta(pto_start, pto_end, dest);
    LOG("[gatt] PTO: start=%u end=%u dest=%s img=%u bytes\n",
                   (unsigned)pto_start, (unsigned)pto_end,
                   dest, (unsigned)img_len);

    // Post photo event (img_buf=nullptr, img_len=0 → clear cache).
    ble_post_pto_photo_event(img_buf, img_len);
}

} // namespace

namespace gatt_server {

void init() {
    NimBLEServer* server = NimBLEDevice::createServer();
    // Server callbacks are handled in ble_manager.cpp (connection events).

    NimBLEService* svc = server->createService(SVC_UUID);

    // 0001 Protocol Version — Read, no encryption required
    c_proto_ver = svc->createCharacteristic(
        "6F726900-0001-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ);
    c_proto_ver->setCallbacks(&s_char_cb);
    {
        uint8_t buf[64];
        size_t  n = cbor_encode_proto_ver(buf, sizeof(buf));
        c_proto_ver->setValue(buf, n);
    }

    // 0002 Device Status — Read + Notify, no encryption required
    c_dev_status = svc->createCharacteristic(
        "6F726900-0002-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    c_dev_status->setCallbacks(&s_char_cb);
    c_dev_status->setValue(&g_device_status, 1);

    // SECURITY NOTE: All characteristics below use WRITE_ENC / READ_ENC (any
    // encrypted link) instead of WRITE_AUTHEN / READ_AUTHEN (MITM-protected).
    // This allows Just Works bonding so the Python mock tool can test M5 without
    // implementing passkey entry. After M5 testing is complete, every _ENC flag
    // below MUST be changed to _AUTHEN to close the Just Works bypass vulnerability.
    // See: https://github.com/anthropics/... (tracked in M5 sign-off checklist)

    // 0003 Time Sync — Write with response, encrypted
    c_time_sync = svc->createCharacteristic(
        "6F726900-0003-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    c_time_sync->setCallbacks(&s_char_cb);

    // 0004 Profile Info — Write with response, encrypted
    c_profile = svc->createCharacteristic(
        "6F726900-0004-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    c_profile->setCallbacks(&s_char_cb);

    // 0005 Profile Photo — Write with response, encrypted (chunked)
    c_photo = svc->createCharacteristic(
        "6F726900-0005-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    c_photo->setCallbacks(&s_char_cb);

    // 0006 Meeting List — Write with response, encrypted (chunked)
    c_meetings = svc->createCharacteristic(
        "6F726900-0006-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    c_meetings->setCallbacks(&s_char_cb);

    // 0007 PTO Entry — Write with response, encrypted (chunked)
    c_pto = svc->createCharacteristic(
        "6F726900-0007-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    c_pto->setCallbacks(&s_char_cb);

    // 0008 Sync Control — Write + Notify, encrypted
    c_sync_ctrl = svc->createCharacteristic(
        "6F726900-0008-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
        NIMBLE_PROPERTY::NOTIFY);
    c_sync_ctrl->setCallbacks(&s_char_cb);

    // 0009 Factory Reset Command — Write with response, encrypted
    c_factory_rst = svc->createCharacteristic(
        "6F726900-0009-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    c_factory_rst->setCallbacks(&s_char_cb);

    // 000A Sync Manifest — Write + Notify, encrypted
    c_manifest = svc->createCharacteristic(
        "6F726900-000A-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
        NIMBLE_PROPERTY::NOTIFY);
    c_manifest->setCallbacks(&s_char_cb);

    // 000B Keyboard Command — Notify (encrypted via READ_ENC; no write needed)
    // NimBLE 2.5 does not have NOTIFY_ENC; encryption enforced via READ_ENC.
    c_kbd_cmd = svc->createCharacteristic(
        "6F726900-000B-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
    c_kbd_cmd->setCallbacks(&s_char_cb);

    // 000C Host Volume State — Read + Write, encrypted
    c_host_vol = svc->createCharacteristic(
        "6F726900-000C-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    c_host_vol->setCallbacks(&s_char_cb);

    // 000D Media Metadata — Write + Notify, encrypted
    c_media_meta = svc->createCharacteristic(
        "6F726900-000D-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
    c_media_meta->setCallbacks(&s_char_cb);

    // 000E Media Album Art — Write no response, encrypted (chunked JPEG)
    c_album_art = svc->createCharacteristic(
        "6F726900-000E-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    c_album_art->setCallbacks(&s_char_cb);

    // 000F Presence Status — Read + Write, encrypted
    c_presence = svc->createCharacteristic(
        "6F726900-000F-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    c_presence->setCallbacks(&s_char_cb);
    c_presence->setValue(&g_presence_byte, 1);

    // In NimBLE 2.5, services are started when NimBLEServer::start() is called.
    // svc->start() is deprecated and a no-op; omit it.
    // The server is started in ble_manager::init() after all characteristics are set up.
    LOG("[gatt] Service registered: 15 characteristics\n");
}

bool is_ota_active() { return g_ota_active; }

void set_ota_active(bool active) {
    g_ota_active = active;
    LOG("[gatt] OTA active: %d\n", (int)active);
}

void set_device_status(uint8_t status) {
    g_device_status = status;
    if (c_dev_status) {
        c_dev_status->setValue(&g_device_status, 1);
        c_dev_status->notify();
    }
    LOG("[gatt] DeviceStatus -> 0x%02X\n", (unsigned)status);
}

uint8_t get_device_status() { return g_device_status; }

void notify_keyboard_command(const char* op, uint32_t arg) {
    if (!c_kbd_cmd) return;
    // Encode KeyboardCommand { op, arg }
    CborEncoder enc, map;
    uint8_t buf[64];
    cbor_encoder_init(&enc, buf, sizeof(buf), 0);
    cbor_encoder_create_map(&enc, &map, 2);
    cbor_encode_text_stringz(&map, "op");
    cbor_encode_text_stringz(&map, op);
    cbor_encode_text_stringz(&map, "arg");
    cbor_encode_uint(&map, arg);
    cbor_encoder_close_container(&enc, &map);
    size_t n = cbor_encoder_get_buffer_size(&enc, buf);
    c_kbd_cmd->notify(buf, n);
    LOG("[gatt] KeyboardCommand: op=%s arg=%u\n", op, (unsigned)arg);
}

void abort_sync_stage() {
    if (!g_stage.active && !g_sync_in_progress) return;
    LOG("[gatt] Sync aborted (disconnect) — discarding staged data\n");
    g_sync_in_progress = false;
    stage_reset();
}

// Called from ble_manager::poll() (main task) in response to
// SyncControl{op:"END"}. Applies all staged items to NVS/UI in one burst,
// then transitions Device Status and signals SyncEnd for the UI advance.
void run_staged_commit() {
    // Blank the display immediately before the flash write burst. LCD_CAM DMA
    // keeps scanning the (now black) framebuffer, so there are no rendering
    // glitches during NVS / LittleFS writes. LVGL redraws the next screen
    // automatically when it flushes after the state transition below.
    lcd_panel::blackout();

    stage_commit();

    time_t now = time(nullptr);
    nvs_sync::save_epoch((uint32_t)now);
    app_state::set_last_sync_time(now);

    uint8_t new_status = (g_device_status == DS_SETUP_SYNCING ||
                          g_device_status == DS_SETUP_BONDED_AWAITING_SYNC)
                          ? DS_SETUP_SYNC_COMPLETE
                          : DS_RUNTIME_READY;
    set_device_status(new_status);
    ble_post_sync_end_event();
}

} // namespace gatt_server

// ── Volume swipe state setters (called from screen_media_mode BLE hooks) ───

extern "C" {
void gatt_server_set_vol_swipe_active(bool active) {
    g_vol_swipe_active = active;
    if (!active) {
        g_vol_swipe_end_ms = (uint32_t)millis();
    }
}
}
