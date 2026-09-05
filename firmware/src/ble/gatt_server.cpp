// Ori GATT Server — 22 characteristics, ble-protocol.md v1.0.
//
// Service UUID: 6F726900-0000-4F72-9F00-000000000000
// Each char UUID replaces bytes 4-5 with the offset:
//   0001 Device Status           Read+Notify
//   0002 Time Sync               Write (enc)
//   0003 Profile Info            Write (enc)
//   0004 Profile Photo           Write + Write-NR (enc, chunked)
//   0005 Meeting List            Write + Write-NR (enc, chunked)
//   0006 Time Off Entry          Write + Write-NR (enc, chunked)
//   0007 Sync Control            Write+Notify (enc)
//   0008 Device Command          Write (enc)  — magic-routed: factory reset | unpair phone
//   0009 Sync Manifest           Write+Notify (enc)
//   000A Keyboard Command        Notify (enc)
//   000B Host Volume State       Read+Write (enc)
//   000C Media Metadata          Write+Notify (enc)
//   000D Media Album Art         Write no-rsp (enc, chunked)
//   000E Device Settings         Read+Write (enc) — shortcuts | clock face | ANCS filter
//   000F Phone Bond Status       Read+Notify (enc)
//   0010 ANCS Notification       Read+Notify (enc)
//   0011 ANCS Call State         Read+Notify (enc)
//   0012 ANCS Notification Action Write (enc)
//   0013 Lunar Holiday List      Write no-rsp (enc, chunked) — no Notify (no CCCD slot)
//   0014 Firmware Update Control Read+Write+Notify (enc) — BEGIN/END/ABORT ↔ status
//   0015 Firmware Update Data    Write no-rsp + Write (enc) — offset-framed image bytes
//   0016 ANCS App Filter        Write no-rsp (enc, chunked) — AppPassthrough allowlist
//
// Firmware version no longer rides this service — it's exposed via the BLE
// SIG standard Device Information Service (0x180A) / Firmware Revision
// String characteristic (0x2A26), registered separately in init() below —
// see ble-protocol.md §3/§3.1.

#include "ble/gatt_server.h"
#include "ble/ble_manager.h"
#include "ble/ancs_client.h"   // ANCS_APP_FILTER_MAX_BYTES (char 0016 handler)
#include "ble/chunked_transfer.h"
#include "ble/rssi_util.h"
#include "factory_info.h"
#include "fw_version.h"
#include "ota_receiver.h"

// These functions are defined in ble_manager.cpp and called from the
// GATT write callbacks to post events to the main-task event queue.
// widget_profile_card must be included first for the WeatherCondition type.
#include "widgets/widget_profile_card.h"

void ble_post_factory_reset_event();
void ble_post_clock_face_event(uint8_t face);
void ble_post_time_format_event(uint8_t fmt);
void ble_post_unpair_phone_event();
void ble_post_ancs_filter_event(uint8_t level);
void ble_post_ancs_apps_event(char* packed, size_t len);
void ble_post_weather_event(uint8_t condition, int16_t temp_f, uint8_t unit, bool is_night, uint8_t intensity);
void ble_post_shortcut_update_event();
void ble_post_media_meta_event();
void ble_post_seek_step_event(uint8_t seconds);
void ble_post_host_volume_event(uint8_t level, bool show_toast);
void ble_post_album_art_started_event();
void ble_post_album_art_progress_event(uint8_t pct);
void ble_post_album_art_event(uint8_t* buf, size_t len);
void ble_post_lunar_holidays_event(uint16_t* days, size_t count);
void ble_post_holiday_country_event(uint8_t country);
void ble_post_holiday_region_event(uint8_t region);
void ble_post_photo_event(uint8_t* buf, size_t len);
void ble_post_sync_begin_event(uint32_t total_bytes);
void ble_post_sync_commit_event();
void ble_post_sync_end_event();
void ble_post_orioning_progress(uint8_t pct);
void ble_post_time_off_photo_event(uint8_t* buf, size_t len);
void ble_post_ancs_action_event(uint32_t uid, uint8_t action);
void ble_post_ancs_resubscribed_event(bool call_state);

#include <Arduino.h>
#include "ori_log.h"
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
// Service Changed indication (announce_gatt_layout_change) — the raw NimBLE
// GATT service API, which NimBLE-Arduino neither wraps nor puts on the include
// path (unlike the host API in NimBLEDevice.h, which is where ble_gap_conn_rssi
// and ble_att_mtu come from). Hence the full path from the library's src root.
#include <nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include <string.h>
#include <string>

// ArduinoCBOR — tiny CBOR encoder/decoder.
// Header-only style included via lib_deps.
#include <cbor.h>

#include "holiday_data.h"
#include "nvs_store.h"
#include "nvs_sync.h"
#include "state_machine.h"
#include "app_state.h"
#include "lcd_panel.h"
#include "ui_helpers.h"

// UUIDs ---------------------------------------------------------------------

#define SVC_UUID  "6F726900-0000-4F72-9F00-000000000000"

// Helper: replace bytes 4-5 of base UUID with offset.
// Base: 6F726900-XXXX-4F72-9F00-000000000000
#define CHAR_UUID(n) \
    "6F726900-" #n "-4F72-9F00-000000000000"

// Device Status bytes -------------------------------------------------------
#define DS_SETUP_WAITING_PAIRING       0x00
#define DS_SETUP_BONDED_AWAITING_SYNC  0x01
#define DS_SETUP_SYNCING               0x02
#define DS_SETUP_SYNC_COMPLETE         0x03
#define DS_RUNTIME_READY               0x10
#define DS_RUNTIME_RECONNECTING        0x11
#define DS_RUNTIME_SYNCING             0x12
#define DS_ERROR_GENERIC               0xF0

// Firmware version (for the standard Firmware Revision String characteristic
// + OTA checks). Single source of truth in include/fw_version.h — shared
// with ota_receiver.
#define FIRMWARE_VERSION ORI_FW_VERSION

namespace {

// -- OTA guard ------------------------------------------------------------
volatile bool g_ota_active = false;

// -- Device Status ---------------------------------------------------------
uint8_t g_device_status = DS_SETUP_WAITING_PAIRING;

// -- Characteristic handles ------------------------------------------------
NimBLECharacteristic* c_dev_status  = nullptr; // 0001
NimBLECharacteristic* c_time_sync   = nullptr; // 0002
NimBLECharacteristic* c_profile     = nullptr; // 0003
NimBLECharacteristic* c_photo       = nullptr; // 0004
NimBLECharacteristic* c_meetings    = nullptr; // 0005
NimBLECharacteristic* c_time_off     = nullptr; // 0006
NimBLECharacteristic* c_sync_ctrl   = nullptr; // 0007
NimBLECharacteristic* c_dev_cmd      = nullptr; // 0008
NimBLECharacteristic* c_manifest    = nullptr; // 0009
NimBLECharacteristic* c_kbd_cmd     = nullptr; // 000A
NimBLECharacteristic* c_host_vol    = nullptr; // 000B
NimBLECharacteristic* c_media_meta  = nullptr; // 000C
NimBLECharacteristic* c_album_art   = nullptr; // 000D
NimBLECharacteristic* c_dev_settings = nullptr; // 000E
NimBLECharacteristic* c_phone_status = nullptr; // 000F
NimBLECharacteristic* c_ancs_notif   = nullptr; // 0010
NimBLECharacteristic* c_ancs_call    = nullptr; // 0011
NimBLECharacteristic* c_ancs_action  = nullptr; // 0012
NimBLECharacteristic* c_lunar_holidays = nullptr; // 0013
NimBLECharacteristic* c_ancs_app_filter = nullptr; // 0016
NimBLECharacteristic* c_fw_ctrl      = nullptr; // 0014
NimBLECharacteristic* c_fw_data      = nullptr; // 0015

// Last-encoded Phone Bond Status fields. The characteristic carries all
// nine fields in one CBOR blob, but bond/connection-state changes
// (notify_phone_bond_status) and ANCS-driven stats changes (notify_phone_stats)
// arrive from different call sites at different times — each needs to
// re-encode the fields it doesn't own from these cached values rather than
// clobbering them with defaults.
bool    g_phone_bonded        = false;
bool    g_phone_connected     = false;
char    g_phone_name_cache[64] = {};
// 64, not 32 — must match ancs_client.cpp's own g_phone_device_type[64] and
// the ble-protocol.md §10 wire cap (63 bytes): iphone_model_map.h's
// connectivity/region suffixes ("iPad Pro 12.9-inch (5th gen) — Wi-Fi +
// Cellular (Global)", 58 bytes) don't fit in 32 and were silently truncated
// before this was widened — PhoneBondStatus.d re-encoded from this cache
// (notify_phone_stats()) sent a truncated device name to Orion for any long
// model string, even though the value ancs_client passed in was correct.
char    g_phone_device_type_cache[64] = {};
uint8_t g_phone_missed        = 0;
uint8_t g_phone_unread        = 0;
uint8_t g_phone_total         = 0;
uint8_t g_phone_signal        = 0;
uint8_t g_phone_battery       = 0;

// Meeting list is RAM-only (not persisted to NVS — see state_machine). Its
// delta-sync hash therefore also lives in RAM only: a power cycle drops the
// meetings AND this hash together, so the next reconnect's manifest reports
// "meetings" as needed and Orion re-pushes them (instead of wrongly assuming
// they're still cached). Profile/photo/Time Off hashes stay in NVS (that data
// does persist). Set on each MeetingList commit; consulted in handle_manifest_write.
uint8_t g_meetings_hash[32]  = {};
bool    g_meetings_hash_valid = false;

// -- Chunked transfer contexts ---------------------------------------------
chunked_transfer::Context g_photo_ctx;
chunked_transfer::Context g_meetings_ctx;
chunked_transfer::Context g_time_off_ctx;
chunked_transfer::Context g_art_ctx;
chunked_transfer::Context g_lunar_ctx;
chunked_transfer::Context g_ancs_apps_ctx;
// Debounce for the album-art loading ring's percentage — only post a
// progress event when the integer percent actually changes (same pattern as
// stage_add_bytes()'s g_stage.last_pct_sent below). 0xFF (never a valid
// percent) on seq==0 so the first fragment's percent always posts once.
uint8_t g_art_last_pct_sent = 0xFF;

// -- Sync sequence tracking ------------------------------------------------
uint32_t g_sync_seq = 0;
bool     g_sync_in_progress = false;

// -- Volume state ---------------------------------------------------------
uint8_t g_volume_level = 50;
bool    g_muted = false;

// -- Vertical-swipe override (drag-wins, ble-protocol.md §12) -------------
volatile bool g_vol_swipe_active  = false;
uint32_t      g_vol_swipe_end_ms  = 0;
// Value from the most recent vol_set WE emitted (screen_media_mode's swipe
// release) — lets handle_host_volume() recognize Orion's direct confirmation
// of that command and accept it even inside the post-release ignore window
// below, instead of discarding it as if it were a stale/unrelated push.
uint8_t       g_last_vol_set_value = 0;

// -- Current screen pointer (for setup passkey modal) ---------------------
// Shared from the screen manager; we look it up via state_machine.
// We don't cache it — screen_setup provides show/hide APIs.

// -------------------------------------------------------------------------
// CBOR helpers
// -------------------------------------------------------------------------

// Encode SyncControl { op, seq, reason (optional) }.
static size_t cbor_encode_sync_ctrl(uint8_t* buf, size_t buf_sz,
                                     const char* op, uint32_t seq,
                                     const char* reason = nullptr) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    size_t entries = reason ? 3u : 2u;
    cbor_encoder_create_map(&enc, &map, entries);
    cbor_encode_text_stringz(&map, "o");
    cbor_encode_text_stringz(&map, op);
    cbor_encode_text_stringz(&map, "s");
    cbor_encode_uint(&map, seq);
    if (reason) {
        cbor_encode_text_stringz(&map, "r");
        cbor_encode_text_stringz(&map, reason);
    }
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// Encode Sync Manifest Notify { needed: [text,...] }.
static size_t cbor_encode_manifest_notify(uint8_t* buf, size_t buf_sz,
                                           const char** needed, size_t n) {
    CborEncoder enc, map, arr;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_text_stringz(&map, "n");
    cbor_encoder_create_array(&map, &arr, n);
    for (size_t i = 0; i < n; ++i) {
        cbor_encode_text_stringz(&arr, needed[i]);
    }
    cbor_encoder_close_container(&map, &arr);
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// Opens `data`/`len` as a top-level CBOR map for key-by-key iteration — the
// identical 3-call prologue (parse, verify it's a map, enter the container)
// that begins every CBOR-map write handler below. Returns false (parse
// error, or the top-level value isn't a map) without touching `map_val`;
// each caller keeps its own existing behavior on failure — this only factors
// out the mechanical setup, not any failure handling or key-loop logic.
static bool cbor_open_map(const uint8_t* data, size_t len,
                           CborParser& parser, CborValue& root, CborValue& map_val) {
    if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) return false;
    if (!cbor_value_is_map(&root)) return false;
    cbor_value_enter_container(&root, &map_val);
    return true;
}

// Iterates a CBOR map's key/value pairs — the identical hand-rolled
// "while (!cbor_value_at_end(...))" key-extraction loop that used to be
// duplicated across every characteristic's CBOR parser below. For each
// entry, copies the key into a `KeyBufSize`-byte local buffer and calls
// `handler(key, val)` with `val` positioned on the corresponding value;
// the handler inspects/reads the value (cbor_value_is_*/cbor_value_get_*)
// but must NOT advance `val` itself — this loop advances once per entry,
// both before calling the handler (past a non-string "key" — malformed
// input a compliant peer never sends) and after the handler returns
// (past the value, to the next key).
template <size_t KeyBufSize = 16, typename Handler>
static void for_each_cbor_key(CborValue& map_val, Handler&& handler) {
    while (!cbor_value_at_end(&map_val)) {
        char key[KeyBufSize] = {};
        size_t key_len = sizeof(key) - 1;
        if (cbor_value_is_text_string(&map_val)) {
            cbor_value_copy_text_string(&map_val, key, &key_len, &map_val);
        } else {
            cbor_value_advance(&map_val);
            continue;
        }
        if (cbor_value_at_end(&map_val)) break;

        handler(key, map_val);

        if (!cbor_value_at_end(&map_val)) cbor_value_advance(&map_val);
    }
}

// -- SHA-256 of a CBOR blob (deterministic, for hash-manifest delta) --------

static void sha256_of_buf(const uint8_t* buf, size_t len, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, /*is224=*/0);
    mbedtls_sha256_update(&ctx, buf, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

// True when a freshly-received item hash differs from what's stored for it —
// i.e. Orion needs to (re)send that item. `stored_valid` covers both "no
// stored hash exists yet" (NVS load failed) and "the RAM-only meetings hash
// hasn't been set since boot" (g_meetings_hash_valid). Shared by every case
// in handle_manifest_write()'s profile/photo/meetings/time_off comparisons.
static bool hash_differs(bool stored_valid, const uint8_t stored[32], const uint8_t recv[32]) {
    return !(stored_valid && memcmp(stored, recv, 32) == 0);
}

// -------------------------------------------------------------------------
// Sync staging (PSRAM) — ble-protocol.md §6.0
//
// Everything written between SyncControl{BEGIN} and {END} is held here
// (small fields inline, blob items in PSRAM) instead of touching NVS or
// live UI state. stage_commit() applies it all in one burst at END,
// mirroring the OTA stage-then-flash pattern.
// -------------------------------------------------------------------------

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

    bool     have_time_off = false;
    uint8_t* time_off_cbor = nullptr; // owned (PSRAM, from chunked_transfer)
    size_t   time_off_len  = 0;

};

SyncStage g_stage;

// Forward declarations — apply_* helpers are defined just below stage_commit
// uses them, but stage_reset() needs to free their buffers first.
static void apply_time_sync(uint64_t epoch_utc, const char* tz);
static void apply_profile_cbor(const uint8_t* data, size_t len);
static void apply_photo_jpeg(uint8_t* buf, size_t n);
static void apply_meetings_cbor(uint8_t* buf, size_t n);
static void apply_time_off_cbor(uint8_t* buf, size_t n);

// -- Device Settings (char 000E) parse/apply split -------------------------
// Every field is optional; a present field is validated against its wire
// range (ble-protocol.md §10) before being recorded here. Nothing in `apply`
// runs until the whole map has parsed cleanly — see parse_device_settings().
struct DeviceSettingsWrite {
    bool    has_slots    = false;
    char    slot1[20] = {}, slot2[20] = {}, slot3[20] = {};
    bool    has_clock    = false; uint8_t clock_val = 0;
    bool    has_timefmt  = false; uint8_t timefmt_val = 0;
    bool    has_filter   = false; uint8_t filter_val = 0;
    bool    has_weather_cond = false; uint8_t weather_cond_val = 0;
    bool    has_weather_temp = false; int16_t weather_temp_val = 0;
    bool    has_weather_unit = false; uint8_t weather_unit_val = 0;
    bool    has_weather_night = false; uint8_t weather_night_val = 0;
    bool    has_weather_intensity = false; uint8_t weather_intensity_val = 0;
    bool    has_seek_step = false; uint8_t seek_step_val = 0;
    bool    has_holiday_country = false; uint8_t holiday_country_val = 0;
    bool    has_holiday_region = false; uint8_t holiday_region_val = 0;
    bool    has_work_hours_end = false; uint16_t work_hours_end_val = 0;
    bool    has_work_hours_days = false; uint8_t work_hours_days_val = 0;
    bool    has_weather_alert_en = false; uint8_t weather_alert_en_val = 0;
    bool    has_weather_alert_offset = false; uint8_t weather_alert_offset_val = 0;
    bool    has_low_batt_alert_en = false; uint8_t low_batt_alert_en_val = 0;
    bool    has_low_batt_threshold = false; uint8_t low_batt_threshold_val = 0;
};
// Decodes+validates the Device Settings CBOR map into `out`. Returns false
// if the payload isn't a map, or any present field fails its range check —
// callers must NACK and must not call apply_device_settings() in that case.
static bool parse_device_settings(const uint8_t* data, uint16_t len, DeviceSettingsWrite& out);
// Applies whichever fields were present, in the same fixed order the
// original inline handler used (shortcuts, clock face, time
// format, ANCS filter, weather, seek step) — independent of wire order.
static void apply_device_settings(const DeviceSettingsWrite& w);

// -- Working Hours / Weather Alert / Low Battery Alert config latch --------
// Device Settings keys "o","p","q","t","v","x" (ble-protocol.md §4/§6.4)
// need nothing but a deferred nvs::set_*() call — unlike clock_face/
// holiday_country/etc, there's no LVGL rebuild or app_state reaction tied to
// any of these six, so a dedicated BleEventType per field would be pure
// boilerplate. Mirrors ble_manager.cpp's own progress latches
// (s_art_pct_pending/s_orioning_pct_pending): each field gets its own plain
// "pending" flag + value, written here (NimBLE host task, inside
// apply_device_settings()) and drained by poll_alert_settings() (main task,
// called every tick from ble_manager::poll(), same as
// poll_chunk_timeouts()/poll_orion_signal_bars()) — keeps the NVS flash
// write off the host task (hardware.md's ICache/DCache-disable hazard).
// Six independent flags (not one shared "any pending" bool) so two
// back-to-back writes touching different keys can't clobber each other
// before poll_alert_settings() next drains.
struct AlertSettingsLatch {
    volatile bool work_hours_end_pending      = false;
    uint16_t      work_hours_end_val          = 0;
    volatile bool work_hours_days_pending     = false;
    uint8_t       work_hours_days_val         = 0;
    volatile bool weather_alert_en_pending    = false;
    uint8_t       weather_alert_en_val        = 0;
    volatile bool weather_alert_offset_pending = false;
    uint8_t       weather_alert_offset_val    = 0;
    volatile bool low_batt_alert_en_pending   = false;
    uint8_t       low_batt_alert_en_val       = 0;
    volatile bool low_batt_threshold_pending  = false;
    uint8_t       low_batt_threshold_val      = 0;
};
static AlertSettingsLatch g_alert_latch;

// Free any owned staging buffers and zero the struct.
static void stage_reset() {
    if (g_stage.profile_cbor)  heap_caps_free(g_stage.profile_cbor);
    if (g_stage.photo_jpeg)    heap_caps_free(g_stage.photo_jpeg);
    if (g_stage.meetings_cbor) heap_caps_free(g_stage.meetings_cbor);
    if (g_stage.time_off_cbor) heap_caps_free(g_stage.time_off_cbor);
    g_stage = SyncStage{};
}

// Begin a new staging session (called on SyncControl{BEGIN}).
static void stage_begin(uint32_t total_bytes) {
    // Preserve a Time Sync that arrived moments BEFORE this BEGIN: §6.2's
    // reconnect sequence long documented Time Sync ahead of BEGIN, and
    // handle_time_sync() stages unconditionally — without this carry-over, a
    // sender following that order had its clock silently wiped by the session
    // reset (never applied; on a cold boot that means no clock at all, hidden
    // status-bar time, dead meeting logic). Both current senders (Orion's
    // run_sync, mock_orion_ble.py) write it after BEGIN, where it stages
    // normally — this only rescues the documented-order case. Accuracy is
    // identical either way: even an in-session Time Sync sits staged until
    // END's commit. Gated on !active — a BEGIN that interrupts a LIVE session
    // is a restart, and wiping that session's staged time with the rest of
    // its data remains correct.
    bool     carry_time  = g_stage.have_time && !g_stage.active;
    uint64_t carry_epoch = g_stage.epoch_utc;
    char     carry_tz[sizeof(g_stage.tz)];
    memcpy(carry_tz, g_stage.tz, sizeof(carry_tz));

    stage_reset();
    g_stage.active      = true;
    g_stage.total_bytes = total_bytes;

    if (carry_time) {
        g_stage.have_time = true;
        g_stage.epoch_utc = carry_epoch;
        memcpy(g_stage.tz, carry_tz, sizeof(g_stage.tz));
    }
}

// Account for `n` newly received application-payload bytes (written into the
// PSRAM staging buffers as they arrive) and post byte-accurate progress
// (capped at 99% — 100% is posted only after stage_commit() finishes at
// END). No device-status gate here: a sync session is fully described by
// g_stage.active/total_bytes, and ble_manager fans OrioningProgress out to
// both the setup Orioning ring and the runtime reconnect overlay's ring —
// whichever one isn't actually the live screen no-ops safely on its own.
// (Device status during a runtime sync is DS_RUNTIME_SYNCING, set by the
// BEGIN handler below — gating on DS_RUNTIME_RECONNECTING here, the status
// notified at the BLE-connection moment *before* BEGIN, silently dropped
// every runtime progress update.)
static void stage_add_bytes(uint16_t n) {
    if (!g_stage.active) return;
    g_stage.received_bytes += n;
    if (g_stage.total_bytes == 0) return;

    uint32_t pct = (uint32_t)((uint64_t)g_stage.received_bytes * 100u / g_stage.total_bytes);
    if (pct > 99) pct = 99;
    if ((uint8_t)pct != g_stage.last_pct_sent) {
        g_stage.last_pct_sent = (uint8_t)pct;
        ble_post_orioning_progress((uint8_t)pct);
    }
}

// Registers the on_complete/on_fragment callbacks a chunked staging
// characteristic (Photo, Meetings, Time Off) needs, exactly once per
// context (chunked_transfer::feed() only consults them, it never resets
// them, so re-registering on every call would be harmless but pointless —
// the original per-handler code already guarded on `!ctx.on_complete` the
// same way). `have_flag`/`out_buf`/`out_len` are the SyncStage fields this
// item stages into; `label` is only used in the NACK log line. Captured by
// reference: g_stage is a single perpetual global, never reconstructed at a
// new address (stage_reset()/stage_begin() assign *into* it), so references
// into its fields stay valid for the life of the program.
static void ensure_stage_chunk_callbacks(chunked_transfer::Context& ctx, const char* label,
                                          bool& have_flag, uint8_t*& out_buf, size_t& out_len) {
    if (ctx.on_complete) return;
    ctx.on_complete = [label, &have_flag, &out_buf, &out_len](uint8_t* buf, size_t n, const char* nack) {
        if (nack) {
            LOG("[gatt] %s NACK: %s\n", label, nack);
            return;
        }
        have_flag = true;
        out_buf   = buf;   // ownership moves to SyncStage
        out_len   = n;
    };
    ctx.on_fragment = [](uint16_t /*seq*/, uint16_t /*total*/, uint16_t plen) {
        stage_add_bytes(plen);
    };
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
    if (g_stage.have_time_off) {
        apply_time_off_cbor(g_stage.time_off_cbor, g_stage.time_off_len);
        g_stage.time_off_cbor = nullptr; // freed by apply_time_off_cbor
    }

    if (g_stage.total_bytes > 0) {
        ble_post_orioning_progress(100);
    }

    stage_reset();
}

// Encode PhoneBondStatus CBOR:
//   { "b": bonded, "c": connected, "n": name, "d": device_type,
//     "m": missed_calls, "u": unread_messages, "t": total_notifications,
//     "s": signal_bars (0-4), "l": battery_level (0-100) }
// Stats/signal/battery are always encoded as 0 when connected==false — a
// disconnected iPhone leaves nothing left to verify (same "don't show a
// stale reading" policy as weather, ble-protocol.md §6.4).
// device_type follows name's own treatment (empty string, not zeroed to a
// sentinel) since it's a string field read once per connection, same as name.
static size_t encode_phone_status(uint8_t* buf, size_t buf_sz,
                                   bool bonded, bool connected, const char* name,
                                   const char* device_type,
                                   uint8_t missed, uint8_t unread, uint8_t total,
                                   uint8_t signal_bars, uint8_t battery_level) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 9);
    cbor_encode_text_stringz(&map, "b");
    cbor_encode_boolean(&map, bonded);
    cbor_encode_text_stringz(&map, "c");
    cbor_encode_boolean(&map, connected);
    cbor_encode_text_stringz(&map, "n");
    cbor_encode_text_stringz(&map, name ? name : "");
    cbor_encode_text_stringz(&map, "d");
    cbor_encode_text_stringz(&map, connected && device_type ? device_type : "");
    cbor_encode_text_stringz(&map, "m");
    cbor_encode_uint(&map, connected ? missed : 0);
    cbor_encode_text_stringz(&map, "u");
    cbor_encode_uint(&map, connected ? unread : 0);
    cbor_encode_text_stringz(&map, "t");
    cbor_encode_uint(&map, connected ? total : 0);
    cbor_encode_text_stringz(&map, "s");
    cbor_encode_uint(&map, connected ? signal_bars : 0);
    cbor_encode_text_stringz(&map, "l");
    cbor_encode_uint(&map, connected ? battery_level : 0);
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// -- ANCS relay encoders (chars 0010-0012, ble-protocol.md §13/§4) ---------

// Copy `src` into `dst` (dst_sz-capacity, NUL-terminated), truncating to at
// most dst_sz-1 bytes without splitting a UTF-8 multi-byte sequence — same
// boundary rule as state_machine.cpp's copy_text_truncated(), just operating
// on an in-memory C string (app_state's already-sanitized ANCS text) instead
// of a CborValue being parsed off the wire. Used to bring app_state's stored
// ANCS strings (sized for the on-device overlay) down to the tighter §10
// wire caps before encoding AncsNotification{op:"add"}.
static void utf8_truncate_copy(const char* src, char* dst, size_t dst_sz) {
    if (dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    size_t n = (len < dst_sz - 1) ? len : dst_sz - 1;
    while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) --n;  // don't split a char
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Encode AncsNotification{op:"add", ...} (char 0010). Sent via notify_chunked()
// (below) rather than a single notify(), so — unlike every other
// characteristic in this file — it is NOT bound to fitting one ATT
// notification. Field caps now match Ori's own on-device storage exactly
// (app_state.cpp's AncsDetailEntry — token[32]/display_name[40]/title[193]/
// body[513]/pos_label[33]/neg_label[33], each minus 1 for the null
// terminator), so Orion never shows less than Ori itself does for the same
// notification — see ble-protocol.md §5's "AncsNotification chunking" note
// for the wire-level reasoning. `body` in particular now matches the full
// 512-byte max ANCS itself is asked for (ancs_client.cpp's
// request_attributes()) — nothing left on the table between what the phone
// offers and what Orion ends up showing.
static size_t cbor_encode_ancs_add(uint8_t* buf, size_t buf_sz, uint32_t uid,
                                    const char* icon_token, uint8_t category,
                                    const char* app, const char* title, const char* body,
                                    uint32_t recv_epoch, const char* pos_label,
                                    const char* neg_label, bool has_neg_action, bool silent) {
    char t_icon[32] = {}, t_app[40] = {}, t_title[193] = {}, t_body[513] = {};
    char t_pos[33]  = {}, t_neg[33] = {};
    utf8_truncate_copy(icon_token, t_icon,  sizeof(t_icon));
    utf8_truncate_copy(app,        t_app,   sizeof(t_app));
    utf8_truncate_copy(title,      t_title, sizeof(t_title));
    utf8_truncate_copy(body,       t_body,  sizeof(t_body));
    utf8_truncate_copy(pos_label,  t_pos,   sizeof(t_pos));
    utf8_truncate_copy(neg_label,  t_neg,   sizeof(t_neg));

    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 12);
    cbor_encode_text_stringz(&map, "o"); cbor_encode_text_stringz(&map, "add");
    cbor_encode_text_stringz(&map, "u"); cbor_encode_uint(&map, uid);
    cbor_encode_text_stringz(&map, "k"); cbor_encode_text_stringz(&map, t_icon);
    cbor_encode_text_stringz(&map, "c"); cbor_encode_uint(&map, category);
    cbor_encode_text_stringz(&map, "a"); cbor_encode_text_stringz(&map, t_app);
    cbor_encode_text_stringz(&map, "t"); cbor_encode_text_stringz(&map, t_title);
    cbor_encode_text_stringz(&map, "b"); cbor_encode_text_stringz(&map, t_body);
    cbor_encode_text_stringz(&map, "e"); cbor_encode_uint(&map, recv_epoch);
    cbor_encode_text_stringz(&map, "p"); cbor_encode_text_stringz(&map, t_pos);
    cbor_encode_text_stringz(&map, "n"); cbor_encode_text_stringz(&map, t_neg);
    cbor_encode_text_stringz(&map, "g"); cbor_encode_boolean(&map, has_neg_action);
    cbor_encode_text_stringz(&map, "s"); cbor_encode_boolean(&map, silent);
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// Encode AncsNotification{op:"remove", u:uid} (char 0010).
static size_t cbor_encode_ancs_remove(uint8_t* buf, size_t buf_sz, uint32_t uid) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 2);
    cbor_encode_text_stringz(&map, "o"); cbor_encode_text_stringz(&map, "remove");
    cbor_encode_text_stringz(&map, "u"); cbor_encode_uint(&map, uid);
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// Encode AncsNotification{op:"clear"} (char 0010).
static size_t cbor_encode_ancs_clear(uint8_t* buf, size_t buf_sz) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_text_stringz(&map, "o"); cbor_encode_text_stringz(&map, "clear");
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// Encode AncsCallState{st, u:uid, e:elapsed_s, a,t,p,n,g} (char 0011).
// app/title/pos_label/neg_label truncate to the same wire caps as
// AncsNotification's "a"/"t"/"p"/"n" fields (§10) — a caller name is
// functionally a notification title, same size budget.
static size_t cbor_encode_ancs_call_state(uint8_t* buf, size_t buf_sz,
                                           uint8_t st, uint32_t uid, uint32_t elapsed_s,
                                           const char* app, const char* title,
                                           const char* pos_label, const char* neg_label,
                                           bool has_neg_action, const char* icon_token) {
    char t_app[25] = {}, t_title[33] = {}, t_pos[13] = {}, t_neg[13] = {}, t_tok[25] = {};
    utf8_truncate_copy(app,        t_app,   sizeof(t_app));
    utf8_truncate_copy(title,      t_title, sizeof(t_title));
    utf8_truncate_copy(pos_label,  t_pos,   sizeof(t_pos));
    utf8_truncate_copy(neg_label,  t_neg,   sizeof(t_neg));
    utf8_truncate_copy(icon_token, t_tok,   sizeof(t_tok));

    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, buf_sz, 0);
    cbor_encoder_create_map(&enc, &map, 9);
    cbor_encode_text_stringz(&map, "st"); cbor_encode_uint(&map, st);
    cbor_encode_text_stringz(&map, "u");  cbor_encode_uint(&map, uid);
    cbor_encode_text_stringz(&map, "e");  cbor_encode_uint(&map, elapsed_s);
    cbor_encode_text_stringz(&map, "a");  cbor_encode_text_stringz(&map, t_app);
    cbor_encode_text_stringz(&map, "t");  cbor_encode_text_stringz(&map, t_title);
    cbor_encode_text_stringz(&map, "p");  cbor_encode_text_stringz(&map, t_pos);
    cbor_encode_text_stringz(&map, "n");  cbor_encode_text_stringz(&map, t_neg);
    cbor_encode_text_stringz(&map, "g");  cbor_encode_boolean(&map, has_neg_action);
    // "k" — calling app's icon token, same vocabulary as AncsNotification's "k"
    // (ble-protocol.md §13). Lets Orion render the real app icon for the call.
    cbor_encode_text_stringz(&map, "k");  cbor_encode_text_stringz(&map, t_tok);
    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

// -------------------------------------------------------------------------
// Characteristic write callbacks (NimBLE calls these from its own task)
// -------------------------------------------------------------------------

// Guard: return false (NACK) if OTA is active, the link is not encrypted, or
// the writer isn't the bonded Orion peer. Every characteristic in this
// service is Orion-only (ble-protocol.md §3) — the iPhone bond exists solely
// so Ori can read the iPhone's ANCS service as a *client*; it must never be
// able to write here. Without this check, any bonded peer (i.e. the iPhone)
// could reach e.g. Device Command and trigger a factory reset.
// Peer check shared by both guards below: encrypted link, and the writer is
// the bonded Orion peer. Every characteristic in this service is Orion-only
// (ble-protocol.md §3) — the iPhone bond exists solely so Ori can read the
// iPhone's ANCS service as a *client*; it must never be able to write here.
// Without this check, any bonded peer (i.e. the iPhone) could reach e.g.
// Device Command and trigger a factory reset — or, now, push firmware.
static bool check_orion_writer(NimBLEConnInfo& info, const char* char_name) {
    if (!info.isEncrypted()) {
        LOG("[gatt] NACK %s: not encrypted\n", char_name);
        return false;
    }
    if (info.getConnHandle() != ble_manager::orion_conn_handle()) {
        LOG("[gatt] NACK %s: writer is not the bonded Orion peer\n", char_name);
        return false;
    }
    return true;
}

static bool check_write_allowed(NimBLEConnInfo& info, const char* char_name) {
    if (g_ota_active) {
        LOG("[gatt] NACK %s: firmware update active\n", char_name);
        return false;
    }
    return check_orion_writer(info, char_name);
}

// Guard for the two firmware-update characteristics (0014/0015). Same peer
// requirement as everything else — the bond IS the authority to overwrite the
// app partition, now that there is no physical port standing in for it — but
// deliberately NOT gated on g_ota_active: these are the only writes that must
// keep working for the whole duration of an update.
static bool check_fw_write_allowed(NimBLEConnInfo& info, const char* char_name) {
    return check_orion_writer(info, char_name);
}

// -- Characteristic callbacks (one class per char, or a shared dispatcher) -

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
        } else if (c == c_time_off) {
            handle_time_off(data, len, info);
        } else if (c == c_sync_ctrl) {
            handle_sync_ctrl(data, len, info);
        } else if (c == c_dev_cmd) {
            handle_device_cmd(data, len, info);
        } else if (c == c_manifest) {
            handle_manifest_write(data, len, info);
        } else if (c == c_host_vol) {
            handle_host_volume(data, len, info);
        } else if (c == c_media_meta) {
            handle_media_metadata(data, len, info);
        } else if (c == c_album_art) {
            handle_album_art(data, len, info);
        } else if (c == c_lunar_holidays) {
            handle_lunar_holidays(data, len, info);
        } else if (c == c_ancs_app_filter) {
            handle_ancs_app_filter(data, len, info);
        } else if (c == c_dev_settings) {
            handle_device_settings(data, len, info);
        } else if (c == c_ancs_action) {
            handle_ancs_action(data, len, info);
        } else if (c == c_fw_ctrl) {
            if (!check_fw_write_allowed(info, "FwUpdateControl")) return;
            ota_receiver::on_control_write(data, len);
        } else if (c == c_fw_data) {
            if (!check_fw_write_allowed(info, "FwUpdateData")) return;
            ota_receiver::on_data_write(data, len);
        }
    }

    void onRead(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        if (c == c_dev_status) {
            c->setValue(&g_device_status, 1);
        } else if (c == c_host_vol) {
            if (!info.isEncrypted()) return;
            handle_host_volume_read(c);
        } else if (c == c_dev_settings) {
            if (!info.isEncrypted()) return;
            handle_device_settings_read(c);
        }
    }

    // Fires whenever a central writes either char's CCCD to enable/disable
    // notifications — including on every ordinary reconnect: Orion's own
    // watcher tasks (orion-sync's ancs_notification_watcher/
    // ancs_call_state_watcher) are freshly spawned per-connection and always
    // call subscribe() on startup, so this fires again each time regardless
    // of whether NimBLE's own bonded-CCCD persistence would otherwise have
    // let it skip the write. This is the ONLY correctly-timed signal for
    // "Orion can now actually receive a notify on this characteristic" —
    // unlike the OrionConnected event (encryption complete), which fires
    // long before Orion has even started run_sync, let alone spawned these
    // watcher tasks; a resync notify sent that early is silently dropped by
    // the BLE stack (not subscribed yet) and never arrives. subValue==0 is
    // an unsubscribe (link tearing down) — nothing to resync there.
    // Deferred to the main task (ble_post_ancs_resubscribed_event) for the
    // same reason as handle_ancs_action(): the resync functions read
    // ancs_client's queue/call-state, which is only safe to touch from the
    // main task that owns it, not this NimBLE host-task callback.
    void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
        if (subValue == 0) return;
        if (c == c_ancs_notif) {
            ble_post_ancs_resubscribed_event(/*call_state=*/false);
        } else if (c == c_ancs_call) {
            ble_post_ancs_resubscribed_event(/*call_state=*/true);
        }
    }

    // Notifies Device Settings (char 000E) whenever Ori's own live
    // signal_bars ("r") to Orion changes — mirrors ancs_client.cpp's
    // identical RSSI-poll-and-notify-on-change pattern for the iPhone link
    // (Phone Bond Status "s"). This is what lets Orion's Ori Info modal show
    // live signal bars without polling a BLE read on its own side
    // (ble-protocol.md §4/§6.4, pc-app.md). Public (unlike
    // read_orion_signal_bars()/handle_device_settings_read() below) since
    // gatt_server::poll_orion_signal_bars() calls it on s_char_cb from
    // outside this class. Internally gated to a 5 s interval — call
    // unconditionally, every ble_manager::poll() tick.
    void poll_orion_signal_bars() {
        if (!c_dev_settings || !ble_manager::is_orion_connected()) {
            // Nothing to track (or verify) while disconnected — reset the
            // sentinel so the next connection's first poll always fires,
            // same "don't carry a stale reading across a reconnect" rule
            // Phone Bond Status's own signal cache follows on an iPhone
            // disconnect.
            g_last_notified_orion_signal_bars = 0xFF;
            return;
        }
        constexpr uint32_t POLL_INTERVAL_MS = 5000;
        static uint32_t s_last_poll_ms = 0;
        uint32_t now_ms = (uint32_t)millis();
        if (now_ms - s_last_poll_ms < POLL_INTERVAL_MS) return;
        s_last_poll_ms = now_ms;

        uint8_t bars = read_orion_signal_bars();
        if (bars == g_last_notified_orion_signal_bars) return;
        g_last_notified_orion_signal_bars = bars;
        handle_device_settings_read(c_dev_settings);
        c_dev_settings->notify();
    }

private:

    // -- Time Sync (char 0002) -----------------------------------------------
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
        for_each_cbor_key<32>(map_val, [&](const char* key, CborValue& val) {
            if (strcmp(key, "u") == 0 && cbor_value_is_unsigned_integer(&val)) {
                cbor_value_get_uint64(&val, &epoch_utc);
            } else if (strcmp(key, "z") == 0 && cbor_value_is_text_string(&val)) {
                size_t tz_len = sizeof(tz) - 1;
                cbor_value_copy_text_string(&val, tz, &tz_len, nullptr);
            }
        });

        if (epoch_utc > 0) {
            g_stage.have_time = true;
            g_stage.epoch_utc = epoch_utc;
            strncpy(g_stage.tz, tz, sizeof(g_stage.tz) - 1);
        }
    }

    // -- Profile Info (char 0003) --------------------------------------------
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

    // -- Profile Photo (char 0004) — chunked --------------------------------
    // Staged — hashed and posted at SyncControl{END} via apply_photo_jpeg() (§6.0).
    void handle_photo(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "ProfilePhoto")) return;

        ensure_stage_chunk_callbacks(g_photo_ctx, "Photo",
                                      g_stage.have_photo, g_stage.photo_jpeg, g_stage.photo_len);
        chunked_transfer::feed(&g_photo_ctx, data, len);
    }

    // -- Meeting List (char 0005) — chunked ---------------------------------
    // Staged — hashed and applied at SyncControl{END} via apply_meetings_cbor() (§6.0).
    void handle_meetings(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "MeetingList")) return;

        ensure_stage_chunk_callbacks(g_meetings_ctx, "Meetings",
                                      g_stage.have_meetings, g_stage.meetings_cbor, g_stage.meetings_len);
        chunked_transfer::feed(&g_meetings_ctx, data, len);
    }

    // -- Time Off Entry (char 0006) — chunked -------------------------------
    // Staged — hashed, parsed, and applied at SyncControl{END} via apply_time_off_cbor() (§6.0).
    void handle_time_off(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "TimeOffEntry")) return;

        ensure_stage_chunk_callbacks(g_time_off_ctx, "Time Off",
                                      g_stage.have_time_off, g_stage.time_off_cbor, g_stage.time_off_len);
        chunked_transfer::feed(&g_time_off_ctx, data, len);
    }

    // -- Sync Control (char 0007) --------------------------------------------
    void handle_sync_ctrl(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "SyncControl")) return;

        CborParser parser;
        CborValue  root, map_val;
        if (!cbor_open_map(data, len, parser, root, map_val)) return;

        char     op[8]  = {};
        uint64_t seq    = 0;
        uint64_t total  = 0;

        for_each_cbor_key(map_val, [&](const char* key, CborValue& val) {
            if (strcmp(key, "o") == 0 && cbor_value_is_text_string(&val)) {
                size_t sz = sizeof(op) - 1;
                cbor_value_copy_text_string(&val, op, &sz, nullptr);
            } else if (strcmp(key, "s") == 0 && cbor_value_is_unsigned_integer(&val)) {
                cbor_value_get_uint64(&val, &seq);
            } else if (strcmp(key, "t") == 0 && cbor_value_is_unsigned_integer(&val)) {
                cbor_value_get_uint64(&val, &total);
            }
        });

        LOG("[gatt] SyncControl op=%s seq=%u total=%u\n", op, (unsigned)seq, (unsigned)total);

        if (strcmp(op, "BEGIN") == 0) {
            // Handshake: a valid SyncControl{BEGIN} on the encrypted link proves
            // the bonded peer is the Orion app. If it's the provisional Orion
            // (Step 2), this commits the bond; otherwise it's a no-op.
            ble_manager::confirm_orion_peer();
            g_sync_seq         = (uint32_t)seq;
            g_sync_in_progress = true;
            stage_begin((uint32_t)total);
            {
                uint8_t new_status = (g_device_status <= DS_SETUP_SYNC_COMPLETE)
                                      ? DS_SETUP_SYNCING
                                      : DS_RUNTIME_SYNCING;
                gatt_server::set_device_status(new_status);
            }
            // Defer orioning modal / reconnect overlay to main task (LVGL must
            // not be called from NimBLE task).
            ble_post_sync_begin_event((uint32_t)total);

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

    // -- Device Command (char 0008) — magic-routed --------------------------
    // Accepts three 4-byte magic payloads; all others → NACK_BAD_MAGIC.
    //   0xFA 0xC7 0x5E 0x5E  Factory Reset
    //   0x55 0x4E 0x50 0x52  Unpair Phone ("UNPR")
    //   0x52 0x53 0x59 0x4E  Resync ANCS relay ("RSYN") — see below
    // IMPORTANT: do not touch NVS or LVGL from this NimBLE host-task callback.
    // All actions are deferred to the main-task event queue.
    void handle_device_cmd(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "DeviceCommand")) return;
        if (len < 4) return;

        if (data[0] == 0xFA && data[1] == 0xC7 &&
            data[2] == 0x5E && data[3] == 0x5E) {
            LOG("[gatt] DeviceCommand: factory reset\n");
            ble_post_factory_reset_event();
        } else if (data[0] == 0x55 && data[1] == 0x4E &&
                   data[2] == 0x50 && data[3] == 0x52) {
            LOG("[gatt] DeviceCommand: iPhone unpair\n");
            ble_post_unpair_phone_event();
        } else if (data[0] == 0x52 && data[1] == 0x53 &&
                   data[2] == 0x59 && data[3] == 0x4E) {
            // "RSYN" — Orion explicitly requesting a full ANCS relay replay
            // (chars 0010/0011), sent once its notify pipeline (receivers +
            // WinRT ValueChanged handlers + subscriptions) is fully up
            // (ble-protocol.md §13). PULL-based on purpose: every push-timed
            // resync (the onSubscribe-triggered one, which on a bonded
            // reconnect fires from NimBLE's own bonding-restore before the
            // central is even listening — see firmware.md) lost the race on
            // real hardware, and guessing "late enough" server-side is
            // fragile by nature. Orion asking when IT is ready is correct by
            // construction. Reuses the exact deferred events onSubscribe()
            // already posts — same main-task resync path, different trigger.
            LOG("[gatt] DeviceCommand: ANCS relay resync requested\n");
            ble_post_ancs_resubscribed_event(/*call_state=*/false);
            ble_post_ancs_resubscribed_event(/*call_state=*/true);
        } else {
            LOG("[gatt] DeviceCommand: bad magic\n");
            uint8_t buf[64];
            size_t  n = cbor_encode_sync_ctrl(buf, sizeof(buf),
                                               "NACK", g_sync_seq, "NACK_BAD_MAGIC");
            c_sync_ctrl->notify(buf, n);
        }
    }

    // -- Sync Manifest Write (char 0009) — central?peripheral ---------------
    void handle_manifest_write(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "SyncManifest")) return;

        CborParser parser;
        CborValue  root, map_val;
        if (!cbor_open_map(data, len, parser, root, map_val)) return;

        // Received hashes from Orion. Device Settings (shortcuts) has
        // no entry here — written outside BEGIN/END, never staged (§6.0/§6.4).
        uint8_t recv_profile[32]   = {};
        uint8_t recv_photo[32]     = {};
        uint8_t recv_meetings[32]  = {};
        uint8_t recv_time_off[32]  = {};
        bool    got_profile  = false, got_photo    = false;
        bool    got_meetings = false, got_time_off = false;

        for_each_cbor_key<20>(map_val, [&](const char* key, CborValue& val) {
            if (!cbor_value_is_byte_string(&val)) return;
            size_t sz = 32;
            if (strcmp(key, "p") == 0) {
                cbor_value_copy_byte_string(&val, recv_profile, &sz, nullptr);
                got_profile = (sz == 32);
            } else if (strcmp(key, "h") == 0) {
                cbor_value_copy_byte_string(&val, recv_photo, &sz, nullptr);
                got_photo = (sz == 32);
            } else if (strcmp(key, "m") == 0) {
                cbor_value_copy_byte_string(&val, recv_meetings, &sz, nullptr);
                got_meetings = (sz == 32);
            } else if (strcmp(key, "t") == 0) {
                cbor_value_copy_byte_string(&val, recv_time_off, &sz, nullptr);
                got_time_off = (sz == 32);
            }
        });

        // Compare against stored hashes.
        uint8_t stored[32];
        const char* needed[4];
        size_t needed_count = 0;

        if (got_profile) {
            bool valid = nvs_sync::load_hash(nvs_sync::HASH_KEY_PROFILE, stored);
            if (hash_differs(valid, stored, recv_profile)) needed[needed_count++] = "profile";
        }
        if (got_photo) {
            bool valid = nvs_sync::load_hash(nvs_sync::HASH_KEY_PHOTO, stored);
            if (hash_differs(valid, stored, recv_photo)) needed[needed_count++] = "photo";
        }
        if (got_meetings) {
            // Meetings are RAM-only: compare against the RAM hash, not NVS. After
            // a power cycle g_meetings_hash_valid is false → always "needed".
            if (hash_differs(g_meetings_hash_valid, g_meetings_hash, recv_meetings))
                needed[needed_count++] = "meetings";
        }
        if (got_time_off) {
            bool valid = nvs_sync::load_hash(nvs_sync::HASH_KEY_TIME_OFF, stored);
            if (hash_differs(valid, stored, recv_time_off)) needed[needed_count++] = "to";
        }

        // Notify Orion what we need.
        uint8_t buf[256];
        size_t  n = cbor_encode_manifest_notify(buf, sizeof(buf),
                                                  needed, needed_count);
        c_manifest->notify(buf, n);

        LOG("[gatt] Manifest: need %u items\n", (unsigned)needed_count);
    }

    // -- Host Volume State (char 000B) ---------------------------------------
    void handle_host_volume(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "HostVolume")) return;

        CborParser parser;
        CborValue  root, map_val;
        if (!cbor_open_map(data, len, parser, root, map_val)) return;

        uint64_t level = g_volume_level;
        bool     mute  = g_muted;

        for_each_cbor_key(map_val, [&](const char* key, CborValue& val) {
            if (strcmp(key, "l") == 0 && cbor_value_is_unsigned_integer(&val)) {
                cbor_value_get_uint64(&val, &level);
            } else if (strcmp(key, "m") == 0 && cbor_value_is_boolean(&val)) {
                cbor_value_get_boolean(&val, &mute);
            }
        });
        uint8_t new_level = (uint8_t)(level > 100 ? 100 : level);

        // Drag-wins override: ignore incoming pushes during/shortly after a
        // swipe — UNLESS this push's level exactly matches the value we
        // ourselves just told Orion to set (g_last_vol_set_value). Without
        // that exception, Orion's own confirmation write-back — sent in
        // direct, near-instant response to the vol_set we just emitted at
        // release — lands inside this very window almost every time (a BLE
        // round trip is far faster than 800 ms) and gets discarded here, so
        // g_volume_level (what Orion reads back on reconnect) never reflects
        // a swipe-driven change. A push whose value DOESN'T match this is
        // still rejected as before — it's either stale or an unrelated
        // overlapping change, and the locally-computed swipe value (already
        // applied via set_volume_visual) stays authoritative for the HUD
        // until the window clears.
        uint32_t now = (uint32_t)millis();
        bool in_override_window = g_vol_swipe_active || (now - g_vol_swipe_end_ms < 800);
        bool is_own_echo = (new_level == g_last_vol_set_value);
        if (in_override_window && !is_own_echo) {
            LOG("[gatt] HostVolume: ignoring during swipe override\n");
            return;
        }

        g_volume_level = new_level;
        g_muted        = mute;

        // Update app_state so the media mode screen reflects it. The LVGL
        // touch itself (fill height/label, and — for a genuine external
        // change — surfacing the HUD if it isn't already showing) has to
        // happen on the main task, deferred via event, same reasoning as
        // every other characteristic here.
        app_state::set_media_volume((int)g_volume_level);
        LOG("[gatt] HostVolume: level=%u mute=%d\n",
                       (unsigned)g_volume_level, (int)g_muted);

        // Only surface the HUD as a "volume changed externally" toast when
        // this ISN'T our own swipe's echo — a swipe already showed (and hid)
        // the HUD locally around the gesture itself; re-flashing it right
        // after would be redundant. is_own_echo is only meaningful while
        // in_override_window (outside that window every accepted push is by
        // definition external — nothing local to have echoed).
        bool show_toast = !(in_override_window && is_own_echo);
        ble_post_host_volume_event(g_volume_level, show_toast);
    }

    void handle_host_volume_read(NimBLECharacteristic* c) {
        // Encode { level, mute } and set the characteristic value.
        CborEncoder enc, map;
        uint8_t buf[32];
        cbor_encoder_init(&enc, buf, sizeof(buf), 0);
        cbor_encoder_create_map(&enc, &map, 2);
        cbor_encode_text_stringz(&map, "l");
        cbor_encode_uint(&map, g_volume_level);
        cbor_encode_text_stringz(&map, "m");
        cbor_encode_boolean(&map, g_muted);
        cbor_encoder_close_container(&enc, &map);
        size_t n = cbor_encoder_get_buffer_size(&enc, buf);
        c->setValue(buf, n);
    }

    // -- Media Metadata (char 000C) ------------------------------------------
    void handle_media_metadata(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "MediaMetadata")) return;

        CborParser parser;
        CborValue  root, map_val;
        if (!cbor_open_map(data, len, parser, root, map_val)) return;

        char     title[193]  = {};
        char     artist[97]  = {};
        bool     can_seek    = false;
        bool     playing     = false;
        bool     has_playing = false;
        uint64_t position_s  = 0;
        uint64_t duration_s  = 0;
        bool     has_seek    = false;

        for_each_cbor_key(map_val, [&](const char* key, CborValue& val) {
            if (strcmp(key, "t") == 0 && cbor_value_is_text_string(&val)) {
                size_t sz = sizeof(title) - 1;
                cbor_value_copy_text_string(&val, title, &sz, nullptr);
            } else if (strcmp(key, "a") == 0 && cbor_value_is_text_string(&val)) {
                size_t sz = sizeof(artist) - 1;
                cbor_value_copy_text_string(&val, artist, &sz, nullptr);
            } else if (strcmp(key, "c") == 0 && cbor_value_is_boolean(&val)) {
                cbor_value_get_boolean(&val, &can_seek);
            } else if (strcmp(key, "p") == 0 && cbor_value_is_boolean(&val)) {
                cbor_value_get_boolean(&val, &playing);
                has_playing = true;
            } else if (strcmp(key, "o") == 0 && cbor_value_is_unsigned_integer(&val)) {
                cbor_value_get_uint64(&val, &position_s);
                has_seek = true;
            } else if (strcmp(key, "d") == 0 && cbor_value_is_unsigned_integer(&val)) {
                cbor_value_get_uint64(&val, &duration_s);
            }
        });

        LOG("[gatt] MediaMetadata: title='%s' artist='%s' can_seek=%d playing=%d%s\n",
                       title, artist, (int)can_seek,
                       has_playing ? (int)playing : -1,
                       has_seek ? " (seek)" : "");
        // Drop glyphs the UI font can't render (emoji, CJK, …) — track titles
        // routinely contain them.
        char ftitle[193] = {};
        char fartist[97] = {};
        ui::sanitize_text(title,  ftitle,  sizeof(ftitle));
        ui::sanitize_text(artist, fartist, sizeof(fartist));
        // A genuine track change (title actually differs — mirrors
        // screen_media_mode.cpp's own update_meta() rule so a play/pause
        // resend of the SAME title doesn't trip this) invalidates whatever
        // Media Album Art transfer is still reassembling for the PREVIOUS
        // track. Without this, g_art_ctx was only ever reset on a full BLE
        // disconnect — a superseded transfer that Orion doesn't manage to
        // abort before it finishes (small art, ~15-30 KB, can complete in
        // well under a second) would still reach on_complete and decode/
        // display the stale old artwork, overwriting the new track's title
        // that already switched moments earlier. Resetting here means any
        // trailing fragments from the old transfer are simply out-of-sync
        // and dropped (chunked_transfer's existing gap-handling), and the
        // new transfer's own seq==0 starts clean.
        if (strcmp(ftitle, app_state::media().title) != 0) {
            chunked_transfer::reset(&g_art_ctx);
        }
        app_state::set_media_meta(ftitle, fartist, can_seek);
        if (has_playing) app_state::set_media_playing(playing);
        // "o" without "d" is meaningless — require both to be present and
        // duration > 0 before updating the seek position.
        if (has_seek && duration_s > 0)
            app_state::set_media_seek((uint32_t)position_s, (uint32_t)duration_s);
        // app_state writes are plain struct copies (safe from this NimBLE host
        // task), but painting them onto the live screen touches LVGL labels —
        // defer that to the main task.
        ble_post_media_meta_event();
    }

    // -- Media Album Art (char 000D) — chunked raw JPEG ---------------------
    void handle_album_art(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "AlbumArt")) return;

        if (!g_art_ctx.on_complete) {
            g_art_ctx.on_complete = [](uint8_t* buf, size_t n, const char* nack) {
                if (nack) {
                    LOG("[gatt] AlbumArt NACK: %s\n", nack);
                    // Tell the media screen to hide the loading ring — no art
                    // is coming for this attempt (buf=nullptr is the existing
                    // "nothing to show" convention for this event).
                    ble_post_album_art_event(nullptr, 0);
                    return;
                }
                // JPEG decoded via LVGL's TJPGD decoder.
                // The buf is already in PSRAM. Store in g_album_art_psram and
                // signal the media mode screen to reload.
                ble_post_album_art_event(buf, n);
            };
            // Fires after every fragment. seq==0 (the first fragment of a new
            // transfer) is the earliest point we know Orion has actually
            // started sending art, as opposed to showing a loading indicator
            // the instant Controls mode opens (before any data is guaranteed
            // to be coming) — that's when the ring appears. Every fragment
            // also updates the ring's live percentage from seq/total,
            // debounced to one post per integer percent change (same
            // approach as stage_add_bytes()'s sync-progress ring).
            g_art_ctx.on_fragment = [](uint16_t seq, uint16_t total, uint16_t /*plen*/) {
                if (seq == 0) {
                    g_art_last_pct_sent = 0xFF;
                    ble_post_album_art_started_event();
                }
                if (total == 0) return;
                uint8_t pct = (uint8_t)(((uint32_t)(seq + 1) * 100u) / total);
                if (pct > 99) pct = 99;  // 100% is reserved for on_complete
                if (pct != g_art_last_pct_sent) {
                    g_art_last_pct_sent = pct;
                    ble_post_album_art_progress_event(pct);
                }
            };
        }
        chunked_transfer::feed(&g_art_ctx, data, len);
    }

    // -- Lunar Holiday List (char 0013) — chunked CBOR {"e": [uint, ...]} ---
    // No BEGIN/END staging (like Media Album Art) — Orion pushes this once at
    // startup/reconnect and it commits directly on reassembly. CBOR decode
    // happens here on the NimBLE task (pure computation, safe); the actual
    // NVS write is deferred to the main task via ble_post_lunar_holidays_event(),
    // since NVS flash writes disable ICache/DCache briefly (hardware.md) and
    // must never run on the NimBLE host task.
    void handle_lunar_holidays(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "LunarHolidays")) return;

        if (!g_lunar_ctx.on_complete) {
            g_lunar_ctx.on_complete = [](uint8_t* buf, size_t n, const char* nack) {
                if (nack) {
                    LOG("[gatt] LunarHolidays NACK: %s\n", nack);
                    return;
                }
                constexpr size_t MAX_ENTRIES = 200;
                uint16_t days[MAX_ENTRIES];
                size_t count = 0;

                CborParser parser;
                CborValue  root, map_val;
                if (cbor_open_map(buf, n, parser, root, map_val)) {
                    for_each_cbor_key(map_val, [&](const char* key, CborValue& val) {
                        if (strcmp(key, "e") == 0 && cbor_value_is_array(&val)) {
                            CborValue arr;
                            cbor_value_enter_container(&val, &arr);
                            while (!cbor_value_at_end(&arr) && count < MAX_ENTRIES) {
                                if (cbor_value_is_unsigned_integer(&arr)) {
                                    uint64_t v = 0;
                                    cbor_value_get_uint64(&arr, &v);
                                    days[count++] = (uint16_t)v;
                                }
                                cbor_value_advance(&arr);
                            }
                            cbor_value_leave_container(&val, &arr);
                        }
                    });
                }
                heap_caps_free(buf);
                LOG("[gatt] LunarHolidays received: %u entries\n", (unsigned)count);
                ble_post_lunar_holidays_event(days, count);
            };
        }
        chunked_transfer::feed(&g_lunar_ctx, data, len);
    }

    // -- ANCS App Filter (char 0016) — chunked CBOR {"a": [text, ...]} -------
    // The AppPassthrough allowlist: which apps get through while the ANCS
    // filter level (Device Settings "f") is 4. Same shape as Lunar Holidays
    // above — no BEGIN/END staging, no manifest hash, Orion re-pushes it on
    // every (re)connect and it commits straight to NVS on reassembly.
    //
    // Chunked rather than another Device Settings field because it doesn't
    // fit one ATT write: all ~49 compiled-in ancs_icons.h tokens come to
    // roughly 550 bytes against the 244 a 247-byte MTU leaves for a payload.
    //
    // Decoded here on the NimBLE task into the packed NUL-separated layout
    // ancs_client/nvs both use (pure computation, safe); the NVS write and
    // the LVGL-touching refresh are deferred to the main task via
    // ble_post_ancs_apps_event(), which takes ownership of the heap buffer.
    void handle_ancs_app_filter(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "AncsAppFilter")) return;

        if (!g_ancs_apps_ctx.on_complete) {
            g_ancs_apps_ctx.on_complete = [](uint8_t* buf, size_t n, const char* nack) {
                if (nack) {
                    LOG("[gatt] AncsAppFilter NACK: %s\n", nack);
                    return;
                }
                // Packed output can never exceed the CBOR that carried it
                // (every token loses its length header and gains one NUL), so
                // the cap is the only bound worth checking.
                char*  packed = (char*)heap_caps_malloc(
                    ancs_client::ANCS_APP_FILTER_MAX_BYTES, MALLOC_CAP_8BIT);
                size_t used   = 0;
                size_t apps   = 0;
                if (!packed) {
                    LOG("[gatt] AncsAppFilter: alloc failed\n");
                    heap_caps_free(buf);
                    return;
                }

                CborParser parser;
                CborValue  root, map_val;
                if (cbor_open_map(buf, n, parser, root, map_val)) {
                    for_each_cbor_key(map_val, [&](const char* key, CborValue& val) {
                        if (strcmp(key, "a") != 0 || !cbor_value_is_array(&val)) return;
                        CborValue arr;
                        cbor_value_enter_container(&val, &arr);
                        while (!cbor_value_at_end(&arr)) {
                            if (cbor_value_is_text_string(&arr)) {
                                // +1 for the NUL this token needs; stop cleanly
                                // at the cap rather than writing a partial token.
                                char   tok[32] = {};
                                size_t tsz     = sizeof(tok) - 1;
                                if (cbor_value_copy_text_string(&arr, tok, &tsz, nullptr) == CborNoError) {
                                    size_t tl = strlen(tok);
                                    if (tl > 0 &&
                                        used + tl + 1 <= ancs_client::ANCS_APP_FILTER_MAX_BYTES) {
                                        memcpy(&packed[used], tok, tl);
                                        used += tl;
                                        packed[used++] = '\0';
                                        ++apps;
                                    }
                                }
                            }
                            cbor_value_advance(&arr);
                        }
                        cbor_value_leave_container(&val, &arr);
                    });
                }
                heap_caps_free(buf);
                LOG("[gatt] AncsAppFilter received: %u app(s), %u bytes\n",
                    (unsigned)apps, (unsigned)used);
                ble_post_ancs_apps_event(packed, used);  // ownership transferred
            };
        }
        chunked_transfer::feed(&g_ancs_apps_ctx, data, len);
    }

    // -- Device Settings read ----------------------------------------------------
    // Returns all NVS-persisted fields: "c" (clock_face), "h" (time_format),
    // "f" (ancs_filter), "1"/"2"/"3" (shortcut slot tokens), "k"
    // (seek_step_s), "g"/"j" (holiday_country/region), and "o"/"p"/"q"/"t"/
    // "v"/"x" (Working Hours end+days, Weather Alert enable+offset, Low
    // Battery Alert enable+threshold). Weather is not returned —
    // ephemeral, Orion is the source of truth.
    // Also returns two read-only fields, never accepted on a write:
    // "s" (serial_number), from the separate write-once "factory" NVS
    // partition, and "r" (signal_bars), Ori's own live RSSI to Orion — see
    // ble-protocol.md §4/§6.4, provisioning.md. Manufacture date is NOT a
    // field here (or anywhere on-device): it's the serial's own leading
    // DDMMYY digits, and Orion derives it from "s" instead of being sent a
    // second, redundant copy of the same fact.
    // Live RSSI of the connection this read arrived on (Orion's own link to
    // Ori) — Orion's Windows backend (btleplug) can't read RSSI of an already-
    // connected peripheral, only from advertising packets, which stop the
    // moment Ori is connected (pc-app.md). Ori CAN read it, on either side of
    // any of its own active connections, via the same ble_gap_conn_rssi()
    // primitive the iPhone link polls directly for its own signal bars
    // (ancs_client.cpp) — so Ori samples its own link to Orion fresh on
    // every Device Settings read and hands back the bucketed bar count,
    // mirroring PhoneBondStatus's "s" field instead of adding a dedicated
    // characteristic for it.
    uint8_t read_orion_signal_bars() {
        int8_t rssi = 0;
        int rc = ble_gap_conn_rssi(ble_manager::orion_conn_handle(), &rssi);
        if (rc != 0) return 0;
        return ble::rssi_to_bars(rssi);
    }

    // Last signal_bars value actually notified via poll_orion_signal_bars()
    // below — 0xFF (never a valid 0-4 bucket) is the "haven't notified yet
    // this connection" sentinel, so the very first poll after a connect
    // always fires once even if the real bucket happens to be 0. Mirrors
    // ancs_client.cpp's identical g_signal_bars cache for the iPhone link.
    uint8_t g_last_notified_orion_signal_bars = 0xFF;

    void handle_device_settings_read(NimBLECharacteristic* c) {
        CborEncoder enc, map;
        uint8_t buf[256];
        cbor_encoder_init(&enc, buf, sizeof(buf), 0);
        cbor_encoder_create_map(&enc, &map, 17);
        cbor_encode_text_stringz(&map, "c");
        cbor_encode_uint(&map, (uint64_t)nvs::get_clock_face());
        cbor_encode_text_stringz(&map, "h");
        cbor_encode_uint(&map, (uint64_t)nvs::get_time_format());
        cbor_encode_text_stringz(&map, "f");
        cbor_encode_uint(&map, (uint64_t)nvs::get_notif_filter());
        const app_state::ShortcutSlot* slots = app_state::shortcuts();
        cbor_encode_text_stringz(&map, "1");
        cbor_encode_text_stringz(&map, slots[0].icon_token ? slots[0].icon_token : "");
        cbor_encode_text_stringz(&map, "2");
        cbor_encode_text_stringz(&map, slots[1].icon_token ? slots[1].icon_token : "");
        cbor_encode_text_stringz(&map, "3");
        cbor_encode_text_stringz(&map, slots[2].icon_token ? slots[2].icon_token : "");
        cbor_encode_text_stringz(&map, "k");
        cbor_encode_uint(&map, (uint64_t)app_state::seek_step_s());
        cbor_encode_text_stringz(&map, "g");
        cbor_encode_uint(&map, (uint64_t)nvs::get_holiday_country());
        cbor_encode_text_stringz(&map, "j");
        cbor_encode_uint(&map, (uint64_t)nvs::get_holiday_region());
        // Working Hours end/days + Weather Alert + Low Battery Alert config —
        // NVS-persisted, safe to read directly here (same as every other
        // NVS-backed field above): only WRITES need deferral off the NimBLE
        // host task (poll_alert_settings() below), reads don't touch flash.
        cbor_encode_text_stringz(&map, "o");
        cbor_encode_uint(&map, (uint64_t)nvs::get_work_hours_end_min());
        cbor_encode_text_stringz(&map, "p");
        cbor_encode_uint(&map, (uint64_t)nvs::get_work_hours_days());
        cbor_encode_text_stringz(&map, "q");
        cbor_encode_uint(&map, (uint64_t)nvs::get_weather_alert_enabled());
        cbor_encode_text_stringz(&map, "t");
        cbor_encode_uint(&map, (uint64_t)nvs::get_weather_alert_offset_min());
        cbor_encode_text_stringz(&map, "v");
        cbor_encode_uint(&map, (uint64_t)nvs::get_low_battery_alert_enabled());
        cbor_encode_text_stringz(&map, "x");
        cbor_encode_uint(&map, (uint64_t)nvs::get_low_battery_threshold_pct());
        // Read-only device-identity/link fields — "s" comes from the
        // write-once "factory" NVS partition (factory_info.h, never touched
        // by nvs::factory_reset()); "r" is sampled live above. Neither is
        // ever accepted on a Device Settings WRITE — the write handler below
        // simply never looks for these keys, so a write that includes them
        // is a no-op for them (§4's "unknown keys ignored").
        cbor_encode_text_stringz(&map, "s");
        cbor_encode_text_stringz(&map, factory_info::serial_number());
        cbor_encode_text_stringz(&map, "r");
        cbor_encode_uint(&map, (uint64_t)read_orion_signal_bars());
        cbor_encoder_close_container(&enc, &map);
        size_t n = cbor_encoder_get_buffer_size(&enc, buf);
        c->setValue(buf, n);
    }

    // -- Device Settings (char 000E) — CBOR map, partial-update ---------------
    // Merges Shortcut Config, Clock Face, Time Format, ANCS
    // Filter, Weather, and the double-tap Seek Step into one characteristic.
    // All fields are optional; absent
    // keys leave state unchanged. All present fields are validated before any are
    // applied (atomic).
    // Applied immediately, outside the BEGIN/END staging pipeline — same treatment
    // as Clock Face individually.
    void handle_device_settings(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "DeviceSettings")) return;
        if (len == 0) return;

        DeviceSettingsWrite w;
        if (!parse_device_settings(data, len, w)) {
            uint8_t buf[64];
            size_t n = cbor_encode_sync_ctrl(buf, sizeof(buf), "NACK", 0, "NACK_CBOR_DECODE");
            c_sync_ctrl->notify(buf, n);
            return;
        }

        apply_device_settings(w);
    }

    // -- ANCS Notification Action (char 0012) — remote Answer/Decline/End
    // call/Dismiss/Read-all from Orion (ble-protocol.md §13) ---------------
    // { u: uid, a: 0=Positive, 1=Negative }. IMPORTANT: do not call
    // ancs_client::answer_notification()/dismiss_notification() from this
    // NimBLE host-task callback — both issue a blocking ANCS Control Point
    // write-WITH-RESPONSE, which would deadlock here for the exact reason
    // ancs_client.cpp's NS/DS deferral comment explains (the host task that
    // must process the write's ATT response is the one stuck in this
    // callback). Parse-only here; the actual dispatch (and the "is uid still
    // live" check, which touches ancs_client's queue) is deferred to the
    // main task via ble_post_ancs_action_event().
    void handle_ancs_action(const uint8_t* data, uint16_t len, NimBLEConnInfo& info) {
        if (!check_write_allowed(info, "AncsNotificationAction")) return;

        CborParser parser; CborValue root, map_val;
        bool has_uid = false, has_action = false;
        uint64_t uid_v = 0, action_v = 0;

        if (cbor_open_map(data, len, parser, root, map_val)) {
            for_each_cbor_key<4>(map_val, [&](const char* key, CborValue& val) {
                if (strcmp(key, "u") == 0 && cbor_value_is_unsigned_integer(&val)) {
                    cbor_value_get_uint64(&val, &uid_v);
                    has_uid = true;
                } else if (strcmp(key, "a") == 0 && cbor_value_is_unsigned_integer(&val)) {
                    cbor_value_get_uint64(&val, &action_v);
                    has_action = true;
                }
            });
        }

        if (!has_uid || !has_action || action_v > 1) {
            LOG("[gatt] AncsNotificationAction: malformed write\n");
            uint8_t buf[64];
            size_t n = cbor_encode_sync_ctrl(buf, sizeof(buf), "NACK", g_sync_seq, "NACK_CBOR_DECODE");
            c_sync_ctrl->notify(buf, n);
            return;
        }

        LOG("[gatt] AncsNotificationAction: uid=%u action=%u\n",
            (unsigned)uid_v, (unsigned)action_v);
        ble_post_ancs_action_event((uint32_t)uid_v, (uint8_t)action_v);
    }

};

static OriCharacteristicCallbacks s_char_cb;

// -------------------------------------------------------------------------
// Device Settings (char 000E) parse/apply — applied immediately outside the
// BEGIN/END staging pipeline (unlike the apply_* group below), so the split
// here is purely parse-then-apply, not stage-then-commit.
// -------------------------------------------------------------------------

static bool parse_device_settings(const uint8_t* data, uint16_t len, DeviceSettingsWrite& out) {
    CborParser parser; CborValue root, map_val;
    if (!cbor_open_map(data, len, parser, root, map_val)) return false;

    // `valid` (not an early `return false` from inside the loop) because the
    // generic for_each_cbor_key() handler is void-returning — every field
    // already written into `out` before an invalid one is found stays
    // harmless either way, since the caller discards `out` entirely whenever
    // this returns false.
    bool valid = true;
    for_each_cbor_key<4>(map_val, [&](const char* key, CborValue& val) {
        if (!valid) return;  // a prior field already failed validation
        if (strcmp(key, "1") == 0 && cbor_value_is_text_string(&val)) {
            size_t sz = sizeof(out.slot1) - 1;
            cbor_value_copy_text_string(&val, out.slot1, &sz, nullptr);
            out.has_slots = true;
        } else if (strcmp(key, "2") == 0 && cbor_value_is_text_string(&val)) {
            size_t sz = sizeof(out.slot2) - 1;
            cbor_value_copy_text_string(&val, out.slot2, &sz, nullptr);
            out.has_slots = true;
        } else if (strcmp(key, "3") == 0 && cbor_value_is_text_string(&val)) {
            size_t sz = sizeof(out.slot3) - 1;
            cbor_value_copy_text_string(&val, out.slot3, &sz, nullptr);
            out.has_slots = true;
        } else if (strcmp(key, "c") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 1) { valid = false; return; }
            out.clock_val = (uint8_t)v; out.has_clock = true;
        } else if (strcmp(key, "h") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 1) { valid = false; return; }
            out.timefmt_val = (uint8_t)v; out.has_timefmt = true;
        } else if (strcmp(key, "f") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            // 0=Disabled 1=CallOnly 2=Important 3=AppPassthrough 4=All,
            // ordered narrowest to widest. The allowlist level 3 consults its
            // own characteristic (0016) and is independent of this value — a 3
            // with an empty allowlist is legal, it just passes calls only.
            if (v > 4) { valid = false; return; }
            out.filter_val = (uint8_t)v; out.has_filter = true;
        } else if (strcmp(key, "w") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 6) { valid = false; return; }
            out.weather_cond_val = (uint8_t)v; out.has_weather_cond = true;
        } else if (strcmp(key, "d") == 0 && cbor_value_is_integer(&val)) {
            int64_t v; cbor_value_get_int64(&val, &v);
            if (v < -40 || v > 140) { valid = false; return; }
            out.weather_temp_val = (int16_t)v; out.has_weather_temp = true;
        } else if (strcmp(key, "u") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 1) { valid = false; return; }
            out.weather_unit_val = (uint8_t)v; out.has_weather_unit = true;
        } else if (strcmp(key, "n") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 1) { valid = false; return; }
            out.weather_night_val = (uint8_t)v; out.has_weather_night = true;
        } else if (strcmp(key, "i") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 3) { valid = false; return; }
            out.weather_intensity_val = (uint8_t)v; out.has_weather_intensity = true;
        } else if (strcmp(key, "k") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v < 1 || v > 60) { valid = false; return; }
            out.seek_step_val = (uint8_t)v; out.has_seek_step = true;
        } else if (strcmp(key, "g") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 8) { valid = false; return; }  // holiday_data::Country: 0=None 1=US 2=VN 3=CA 4=GB 5=AU 6=ES 7=MX 8=FR
            out.holiday_country_val = (uint8_t)v; out.has_holiday_country = true;
        } else if (strcmp(key, "j") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            // Generous upper bound (Spain's largest region code, Melilla=19)
            // — the specific value's validity within the current country
            // isn't checked here, per holiday_data.h's own "no meaning under
            // this country is just never matched" design.
            if (v > 19) { valid = false; return; }
            out.holiday_region_val = (uint8_t)v; out.has_holiday_region = true;
        } else if (strcmp(key, "o") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 1439) { valid = false; return; }  // work_hours_end_min: minutes since local midnight
            out.work_hours_end_val = (uint16_t)v; out.has_work_hours_end = true;
        } else if (strcmp(key, "p") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 127) { valid = false; return; }  // work_hours_days: 7-bit mask, bit0=Mon..bit6=Sun
            out.work_hours_days_val = (uint8_t)v; out.has_work_hours_days = true;
        } else if (strcmp(key, "q") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 1) { valid = false; return; }  // weather_alert_enabled
            out.weather_alert_en_val = (uint8_t)v; out.has_weather_alert_en = true;
        } else if (strcmp(key, "t") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 30) { valid = false; return; }  // weather_alert_offset_min
            out.weather_alert_offset_val = (uint8_t)v; out.has_weather_alert_offset = true;
        } else if (strcmp(key, "v") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v > 1) { valid = false; return; }  // low_battery_alert_enabled
            out.low_batt_alert_en_val = (uint8_t)v; out.has_low_batt_alert_en = true;
        } else if (strcmp(key, "x") == 0 && cbor_value_is_unsigned_integer(&val)) {
            uint64_t v; cbor_value_get_uint64(&val, &v);
            if (v < 5 || v > 30) { valid = false; return; }  // low_battery_threshold_pct
            out.low_batt_threshold_val = (uint8_t)v; out.has_low_batt_threshold = true;
        }
    });
    return valid;
}

static void apply_device_settings(const DeviceSettingsWrite& w) {
    if (w.has_slots) {
        // app_state write is safe from NimBLE host task (plain struct copy);
        // screen_media_mode::update_shortcuts() touches LVGL — deferred.
        app_state::set_shortcuts(w.slot1, w.slot2, w.slot3);
        ble_post_shortcut_update_event();
        LOG("[gatt] DeviceSettings: shortcuts=%s,%s,%s\n", w.slot1, w.slot2, w.slot3);
    }
    if (w.has_clock) {
        // NVS write + LVGL deferred to main task via event.
        ble_post_clock_face_event(w.clock_val);
        LOG("[gatt] DeviceSettings: clock_face=0x%02X\n", (unsigned)w.clock_val);
    }
    if (w.has_timefmt) {
        // NVS write + on-screen clock/meeting rebuild deferred to main task.
        ble_post_time_format_event(w.timefmt_val);
        LOG("[gatt] DeviceSettings: time_format=%s\n", w.timefmt_val ? "12h" : "24h");
    }
    if (w.has_filter) {
        // NVS write + ancs_client::set_filter() deferred to main task via event.
        ble_post_ancs_filter_event(w.filter_val);
        LOG("[gatt] DeviceSettings: ancs_filter=0x%02X\n", (unsigned)w.filter_val);
    }
    if (w.has_weather_cond && w.has_weather_temp && w.has_weather_unit &&
        w.has_weather_night && w.has_weather_intensity) {
        // Ephemeral — not persisted, not read back. Only
        // applied when ALL FIVE fields are present in this write
        // (defensive: a message with just some of the five shouldn't
        // half-apply).
        ble_post_weather_event(w.weather_cond_val, w.weather_temp_val, w.weather_unit_val,
                                (bool)w.weather_night_val, w.weather_intensity_val);
        LOG("[gatt] DeviceSettings: weather condition=%u temp_f=%d unit=%u night=%u intensity=%u\n",
            (unsigned)w.weather_cond_val, (int)w.weather_temp_val, (unsigned)w.weather_unit_val,
            (unsigned)w.weather_night_val, (unsigned)w.weather_intensity_val);
    } else if (w.has_weather_cond || w.has_weather_temp || w.has_weather_unit ||
               w.has_weather_night || w.has_weather_intensity) {
        LOG("[gatt] DeviceSettings: weather partial write ignored (w=%d d=%d u=%d n=%d i=%d)\n",
            (int)w.has_weather_cond, (int)w.has_weather_temp, (int)w.has_weather_unit,
            (int)w.has_weather_night, (int)w.has_weather_intensity);
    }
    if (w.has_seek_step) {
        // app_state write deferred to main task via event, same as shortcuts —
        // NVS write + app_state::set_seek_step_s() both belong on the main
        // task (see handle_seek_step_update() in ble_manager.cpp).
        ble_post_seek_step_event(w.seek_step_val);
        LOG("[gatt] DeviceSettings: seek_step_s=%u\n", (unsigned)w.seek_step_val);
    }
    if (w.has_holiday_country) {
        // Independent field (unlike the weather group above) — applied
        // whenever present, same treatment as clock_face/ancs_filter. NVS
        // write deferred to the main task, same reasoning as every other
        // NVS-backed Device Settings field.
        ble_post_holiday_country_event(w.holiday_country_val);
        LOG("[gatt] DeviceSettings: holiday_country=%u\n", (unsigned)w.holiday_country_val);
    }
    if (w.has_holiday_region) {
        // Same independent, always-applied treatment as holiday_country
        // above — Orion sends both together every (re)connect.
        ble_post_holiday_region_event(w.holiday_region_val);
        LOG("[gatt] DeviceSettings: holiday_region=%u\n", (unsigned)w.holiday_region_val);
    }
    // Working Hours / Weather Alert / Low Battery Alert config — six
    // independent fields, each only ever needing a deferred NVS write (no
    // LVGL/app_state reaction), so they ride the alert-settings latch
    // (poll_alert_settings(), below) instead of a dedicated BleEventType per
    // field. "Value first, then pending flag" — same publish order
    // ble_manager.cpp's progress latches use.
    if (w.has_work_hours_end) {
        g_alert_latch.work_hours_end_val = w.work_hours_end_val;
        g_alert_latch.work_hours_end_pending = true;
        LOG("[gatt] DeviceSettings: work_hours_end_min=%u\n", (unsigned)w.work_hours_end_val);
    }
    if (w.has_work_hours_days) {
        g_alert_latch.work_hours_days_val = w.work_hours_days_val;
        g_alert_latch.work_hours_days_pending = true;
        LOG("[gatt] DeviceSettings: work_hours_days=0x%02X\n", (unsigned)w.work_hours_days_val);
    }
    if (w.has_weather_alert_en) {
        g_alert_latch.weather_alert_en_val = w.weather_alert_en_val;
        g_alert_latch.weather_alert_en_pending = true;
        LOG("[gatt] DeviceSettings: weather_alert_enabled=%u\n", (unsigned)w.weather_alert_en_val);
    }
    if (w.has_weather_alert_offset) {
        g_alert_latch.weather_alert_offset_val = w.weather_alert_offset_val;
        g_alert_latch.weather_alert_offset_pending = true;
        LOG("[gatt] DeviceSettings: weather_alert_offset_min=%u\n", (unsigned)w.weather_alert_offset_val);
    }
    if (w.has_low_batt_alert_en) {
        g_alert_latch.low_batt_alert_en_val = w.low_batt_alert_en_val;
        g_alert_latch.low_batt_alert_en_pending = true;
        LOG("[gatt] DeviceSettings: low_battery_alert_enabled=%u\n", (unsigned)w.low_batt_alert_en_val);
    }
    if (w.has_low_batt_threshold) {
        g_alert_latch.low_batt_threshold_val = w.low_batt_threshold_val;
        g_alert_latch.low_batt_threshold_pending = true;
        LOG("[gatt] DeviceSettings: low_battery_threshold_pct=%u\n", (unsigned)w.low_batt_threshold_val);
    }
}

// -------------------------------------------------------------------------
// Sync stage commit helpers — apply one staged item to NVS / live state.
// Called only from stage_commit() at SyncControl{END} (§6.0).
// -------------------------------------------------------------------------

static void apply_time_sync(uint64_t epoch_utc, const char* tz) {
    if (epoch_utc == 0) return;

    struct timeval tv = { .tv_sec = (time_t)epoch_utc, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    if (tz && tz[0]) {
        setenv("TZ", tz, 1);
        tzset();
    }

    // Record that Orion has established a true-UTC clock this session and cache
    // its TZ. The iPhone CTS backup (local wall time, no timezone) reuses this TZ
    // to map its reading back onto the SAME true-UTC basis while Orion is offline,
    // so a CTS re-seed doesn't shift time(nullptr) by the timezone offset and
    // break epoch-relative state (the "last synced" pill). See
    // ancs_client::read_phone_time().
    app_state::set_orion_clock_synced(tz);

    LOG("[gatt] TimeSync: epoch=%llu tz=%s\n",
                   (unsigned long long)epoch_utc, tz ? tz : "");
}

static void apply_profile_cbor(const uint8_t* data, size_t len) {
    CborParser parser;
    CborValue  root, map_val;
    if (!cbor_open_map(data, len, parser, root, map_val)) return;

    // Field limits are 32/32/32/16 chars (Orion-enforced at input). Buffers
    // hold the worst-case UTF-8 byte length (3 bytes/char, e.g. Vietnamese);
    // cbor_value_copy_text_string truncates at sizeof-1 as a defensive cap.
    char name[97]  = {};
    char title[97] = {};
    char email[129]= {};
    char phone[33] = {};

    for_each_cbor_key(map_val, [&](const char* key, CborValue& val) {
        if (strcmp(key, "n") == 0 && cbor_value_is_text_string(&val)) {
            size_t sz = sizeof(name) - 1;
            cbor_value_copy_text_string(&val, name, &sz, nullptr);
        } else if (strcmp(key, "t") == 0 && cbor_value_is_text_string(&val)) {
            size_t sz = sizeof(title) - 1;
            cbor_value_copy_text_string(&val, title, &sz, nullptr);
        } else if (strcmp(key, "e") == 0 && cbor_value_is_text_string(&val)) {
            size_t sz = sizeof(email) - 1;
            cbor_value_copy_text_string(&val, email, &sz, nullptr);
        } else if (strcmp(key, "p") == 0 && cbor_value_is_text_string(&val)) {
            size_t sz = sizeof(phone) - 1;
            cbor_value_copy_text_string(&val, phone, &sz, nullptr);
        }
    });

    // Drop glyphs the UI font can't render (emoji, CJK, …) from the visible
    // fields. The manifest hash below is computed over the RAW CBOR bytes, so
    // filtering the stored display text doesn't desync the reconnect delta.
    char fname[97]   = {};
    char ftitle[97]  = {};
    char femail[129] = {};
    char fphone[33]  = {};
    ui::sanitize_text(name,  fname,  sizeof(fname));
    ui::sanitize_text(title, ftitle, sizeof(ftitle));
    ui::sanitize_text(email, femail, sizeof(femail));
    ui::sanitize_text(phone, fphone, sizeof(fphone));

    // Save to NVS.
    nvs_sync::save_profile(fname, ftitle, femail, fphone);

    // Compute SHA-256 of the raw CBOR bytes and store.
    uint8_t hash[32];
    sha256_of_buf(data, len, hash);
    nvs_sync::save_hash(nvs_sync::HASH_KEY_PROFILE, hash);

    // Update the live profile card.
    widget_profile_card::set_profile(fname, ftitle, femail, fphone);

    LOG("[gatt] ProfileInfo: name=%s title=%s\n", fname, ftitle);
}

static void apply_photo_jpeg(uint8_t* buf, size_t n) {
    uint8_t hash[32];
    sha256_of_buf(buf, n, hash);
    nvs_sync::save_hash(nvs_sync::HASH_KEY_PHOTO, hash);
    LOG("[gatt] Photo received: %u bytes\n", (unsigned)n);
    ble_post_photo_event(buf, n); // ownership transferred to event handler
}

static void apply_meetings_cbor(uint8_t* buf, size_t n) {
    // RAM-only: keep the delta-sync hash in RAM (NOT NVS) so a power cycle drops
    // it with the meetings, forcing a re-request on the next reconnect.
    sha256_of_buf(buf, n, g_meetings_hash);
    g_meetings_hash_valid = true;
    LOG("[gatt] Meetings received: %u bytes (RAM-only)\n", (unsigned)n);
    state_machine::set_meetings_cbor(buf, n);
    heap_caps_free(buf);
}

static void apply_time_off_cbor(uint8_t* buf, size_t n) {
    // Hash the full CBOR payload for delta-sync manifest.
    uint8_t hash[32];
    sha256_of_buf(buf, n, hash);
    nvs_sync::save_hash(nvs_sync::HASH_KEY_TIME_OFF, hash);

    // -- Parse TimeOffEntry CBOR --------------------------------
    // Schema: { s:start, e:end, d:destination, m:image }
    CborParser parser;
    CborValue  root, map_val;
    uint32_t   time_off_start = 0, time_off_end = 0;
    char       dest[49] = {};
    uint8_t*   img_buf   = nullptr;
    size_t     img_len   = 0;

    if (cbor_open_map(buf, n, parser, root, map_val)) {
        for_each_cbor_key(map_val, [&](const char* key, CborValue& val) {
            if (strcmp(key, "s") == 0 &&
                cbor_value_is_unsigned_integer(&val)) {
                uint64_t v = 0;
                cbor_value_get_uint64(&val, &v);
                time_off_start = (uint32_t)v;

            } else if (strcmp(key, "e") == 0 &&
                       cbor_value_is_unsigned_integer(&val)) {
                uint64_t v = 0;
                cbor_value_get_uint64(&val, &v);
                time_off_end = (uint32_t)v;

            } else if (strcmp(key, "d") == 0 &&
                       cbor_value_is_text_string(&val)) {
                size_t dsz = sizeof(dest) - 1;
                cbor_value_copy_text_string(&val, dest, &dsz, nullptr);

            } else if (strcmp(key, "m") == 0 &&
                       cbor_value_is_byte_string(&val)) {
                size_t raw_len = 0;
                cbor_value_get_string_length(&val, &raw_len);
                if (raw_len > 0 && raw_len <= 512 * 1024) {
                    img_buf = static_cast<uint8_t*>(
                        heap_caps_malloc(raw_len,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    if (!img_buf)
                        img_buf = static_cast<uint8_t*>(malloc(raw_len));
                    if (img_buf) {
                        img_len = raw_len;
                        cbor_value_copy_byte_string(
                            &val, img_buf, &img_len, nullptr);
                    }
                }
                // raw_len == 0 ? no image set; img_buf stays nullptr
            }
        });
    }

    heap_caps_free(buf); // free staged CBOR blob

    // Drop glyphs the UI font can't render (emoji, CJK, …) from the displayed
    // destination. Time Off hash (if any) is over raw bytes, so this doesn't desync.
    char fdest[49] = {};
    ui::sanitize_text(dest, fdest, sizeof(fdest));

    nvs_sync::save_time_off_meta(time_off_start, time_off_end, fdest);
    LOG("[gatt] Time Off: start=%u end=%u dest=%s img=%u bytes\n",
                   (unsigned)time_off_start, (unsigned)time_off_end,
                   fdest, (unsigned)img_len);

    // Post photo event (img_buf=nullptr, img_len=0 ? clear cache).
    ble_post_time_off_photo_event(img_buf, img_len);
}

} // namespace

namespace gatt_server {

void init() {
    NimBLEServer* server = NimBLEDevice::createServer();
    // Server callbacks are handled in ble_manager.cpp (connection events).

    // Device Information Service — BLE SIG standard (0x180A). Exposes the
    // Firmware Revision String characteristic (0x2A26) so Orion (or any
    // generic BLE client) can read the running firmware version without a
    // custom characteristic or CBOR decode. Unencrypted, static value — it
    // only changes across a reboot after a firmware update (ota.md). That
    // read is also how Orion CONFIRMS an update installed, since §14's
    // VALIDATED is sent before the flash commit rather than after it.
    NimBLEService* dis_svc = server->createService(NimBLEUUID((uint16_t)0x180A));
    NimBLECharacteristic* c_fw_rev = dis_svc->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A26),
        NIMBLE_PROPERTY::READ);
    c_fw_rev->setValue(FIRMWARE_VERSION);

    NimBLEService* svc = server->createService(SVC_UUID);

    // 0001 Device Status — Read + Notify, no encryption required
    c_dev_status = svc->createCharacteristic(
        "6F726900-0001-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    c_dev_status->setCallbacks(&s_char_cb);
    c_dev_status->setValue(&g_device_status, 1);

    // BISECTION CONCLUDED (2026-07-11): the Orion-drop-on-iPhone-pair
    // regression reproduced with the pool-size flags removed and _AUTHEN
    // restored, and was root-caused to neither — the real bug was NimBLE's
    // persisted-CCCD store overflowing when the iPhone's bond persisted a
    // 9th subscription record (global default cap 8; Orion's 8 notify
    // subscriptions filled it), whose overflow handler unpairs the OLDEST
    // bonded peer: Orion's LTK deleted + live link terminated. Fixed via
    // CONFIG_BT_NIMBLE_MAX_CCCDS in platformio.ini (full write-up there).
    // _AUTHEN vs _ENC was never the cause, so chars 0002-000F stay at
    // WRITE_AUTHEN/READ_AUTHEN — the stronger, MITM-gated setting matching
    // ble-protocol.md's encrypted-characteristic contract. (The separate
    // "AUTHEN checks intermittently flaky on bonded reconnect" concern noted
    // in ble_manager.cpp's onAuthenticationComplete comment remains open,
    // but is a reliability question independent of this bug.)

    // 0002 Time Sync — Write with response, MITM-authenticated
    c_time_sync = svc->createCharacteristic(
        "6F726900-0002-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_time_sync->setCallbacks(&s_char_cb);

    // 0003 Profile Info — Write with response, MITM-authenticated
    c_profile = svc->createCharacteristic(
        "6F726900-0003-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_profile->setCallbacks(&s_char_cb);

    // 0004 Profile Photo — Write + Write-No-Response, MITM-authenticated (chunked).
    c_photo = svc->createCharacteristic(
        "6F726900-0004-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_photo->setCallbacks(&s_char_cb);

    // 0005 Meeting List — Write + Write-No-Response, MITM-authenticated (chunked)
    c_meetings = svc->createCharacteristic(
        "6F726900-0005-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_meetings->setCallbacks(&s_char_cb);

    // 0006 Time Off Entry — Write + Write-No-Response, MITM-authenticated (chunked)
    c_time_off = svc->createCharacteristic(
        "6F726900-0006-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_time_off->setCallbacks(&s_char_cb);

    // 0007 Sync Control — Write + Notify, MITM-authenticated. Also declares
    // base READ (added alongside 0009 below) — see 000A's comment a few
    // lines down for the full WinRT write-up. Orion subscribes to this
    // characteristic's notify for NACKs (tools/mock_orion_ble.py's reference
    // client does) exactly like 000A/0010/0011/000F, so it needs the same
    // fix: without base READ, WinRT silently drops every NACK notify even
    // though Ori sent it and Orion's subscribe() reported success.
    c_sync_ctrl = svc->createCharacteristic(
        "6F726900-0007-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN |
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN |
        NIMBLE_PROPERTY::NOTIFY);
    c_sync_ctrl->setCallbacks(&s_char_cb);

    // 0008 Device Command — Write with response, MITM-authenticated.
    c_dev_cmd = svc->createCharacteristic(
        "6F726900-0008-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_dev_cmd->setCallbacks(&s_char_cb);

    // 0009 Sync Manifest — Write + Notify, MITM-authenticated. Base READ
    // added for the same reason as 0007 above — Orion subscribes to this
    // characteristic's "needed" notify to drive the hash-manifest delta
    // sync (ble-protocol.md §6.2); without base READ, WinRT drops that
    // notify entirely and every reconnect silently falls back to resending
    // everything instead of the intended cheap delta.
    c_manifest = svc->createCharacteristic(
        "6F726900-0009-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN |
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN |
        NIMBLE_PROPERTY::NOTIFY);
    c_manifest->setCallbacks(&s_char_cb);

    // 000A Keyboard Command — Read + Notify, MITM-authenticated.
    // READ is REQUIRED, not optional: Windows' WinRT GATT stack does not
    // raise ValueChanged (deliver notifications) for a characteristic that
    // lacks the base READ property — even though subscribe() succeeds and
    // the CCCD write returns Success. Confirmed empirically 2026-07-11:
    // char 000F (READ|READ_AUTHEN|NOTIFY) delivered fine while chars
    // 000A/0010/0011 (NOTIFY|READ_AUTHEN, no base READ) never fired a single
    // ValueChanged on Orion — the notifies left Ori (rc=0) but WinRT dropped
    // them. Matching 000F's property set fixes delivery. READ_AUTHEN still
    // gates the read behind MITM bonding; the readable value is just the
    // last-notified payload (harmless — Orion only ever subscribes).
    c_kbd_cmd = svc->createCharacteristic(
        "6F726900-000A-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
    c_kbd_cmd->setCallbacks(&s_char_cb);

    // 000B Host Volume State — Read + Write, MITM-authenticated
    c_host_vol = svc->createCharacteristic(
        "6F726900-000B-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN |
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_host_vol->setCallbacks(&s_char_cb);

    // 000C Media Metadata — Write + Notify, MITM-authenticated. Already
    // carried READ_AUTHEN but not base READ — READ_AUTHEN alone is a
    // security *requirement* flag (BLE_GATT_CHR_F_READ_AUTHEN); it does not
    // set the actual advertised READ property bit the way NIMBLE_PROPERTY::
    // READ does (ble_gatts_chr_properties() only ORs in BLE_GATT_CHR_PROP_READ
    // from BLE_GATT_CHR_F_READ). Without that bit this characteristic was in
    // the same silently-broken state 000A/0010/0011 were in before their
    // 2026-07-11 fix: WinRT drops ValueChanged for any Notify characteristic
    // lacking base READ. Adding it here completes what READ_AUTHEN's
    // presence already implied was intended.
    c_media_meta = svc->createCharacteristic(
        "6F726900-000C-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN |
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN |
        NIMBLE_PROPERTY::NOTIFY);
    c_media_meta->setCallbacks(&s_char_cb);

    // 000D Media Album Art — Write no response, MITM-authenticated (chunked JPEG)
    c_album_art = svc->createCharacteristic(
        "6F726900-000D-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_album_art->setCallbacks(&s_char_cb);

    // 0013 Lunar Holiday List — Write no response, MITM-authenticated (chunked
    // CBOR). Same treatment as Media Album Art: no Notify (no CCCD slot
    // consumed — firmware.md's global CCCD-store cap only applies to
    // characteristics a peer subscribes to), no BEGIN/END staging — Orion
    // pushes this once at startup/reconnect (it changes essentially never
    // once computed) and it commits directly to NVS on reassembly.
    c_lunar_holidays = svc->createCharacteristic(
        "6F726900-0013-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_lunar_holidays->setCallbacks(&s_char_cb);

    // 0016 ANCS App Filter — Write no response, MITM-authenticated (chunked
    // CBOR {"a":[token,...]}). Identical treatment to Lunar Holiday List
    // above: no Notify (no CCCD slot), no BEGIN/END staging, commits straight
    // to NVS on reassembly. Carries the AppPassthrough allowlist for ANCS
    // filter level 4 — see ble-protocol.md §4/§6.4.
    c_ancs_app_filter = svc->createCharacteristic(
        "6F726900-0016-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_ancs_app_filter->setCallbacks(&s_char_cb);

    // 000E Device Settings — Read + Write + Notify, MITM-authenticated.
    // Notify added 2026-07-13 (ble-protocol.md §4/§6.4) so Orion's Ori Info
    // modal can show live signal_bars ("r") without polling a BLE read —
    // see gatt_server::poll_orion_signal_bars(). Base READ was already
    // present, satisfying the WinRT notify-delivery requirement (firmware.md
    // — "every NOTIFY characteristic must ALSO declare NIMBLE_PROPERTY::READ").
    c_dev_settings = svc->createCharacteristic(
        "6F726900-000E-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ  | NIMBLE_PROPERTY::READ_AUTHEN |
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN |
        NIMBLE_PROPERTY::NOTIFY);
    c_dev_settings->setCallbacks(&s_char_cb);

    // 000F Phone Bond Status — Read + Notify, MITM-authenticated.
    c_phone_status = svc->createCharacteristic(
        "6F726900-000F-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN |
        NIMBLE_PROPERTY::NOTIFY);
    c_phone_status->setCallbacks(&s_char_cb);
    {
        // Seed with "not bonded, not connected" — updated by notify_phone_bond_status().
        uint8_t buf[64];
        size_t  n = encode_phone_status(buf, sizeof(buf), false, false, "", "", 0, 0, 0, 0, 0);
        c_phone_status->setValue(buf, n);
    }

    // 0010 ANCS Notification — Read + Notify, MITM-authenticated. Individual
    // ANCS notification content relay to Orion's drill-down UI
    // (ble-protocol.md §13); driven by ancs_client via notify_ancs_add()/
    // notify_ancs_remove()/notify_ancs_clear() below.
    //
    // READ is REQUIRED for notification DELIVERY on Windows, not just for
    // reads — see char 000A's comment above for the full write-up. Short
    // version: WinRT GATT silently drops ValueChanged for a characteristic
    // with no base READ property, so char 0010 (previously NOTIFY|READ_AUTHEN)
    // never delivered a single notification to Orion even though Ori sent
    // them and Orion's subscribe succeeded — the exact "count is right but the
    // list is empty" symptom. Matching char 000F's READ|READ_AUTHEN|NOTIFY
    // set fixes it. (Earlier this was reverted _ENC→_AUTHEN during the CCCD-
    // overflow bisection; _AUTHEN is correct and kept — the missing bit was
    // always READ, not the auth level.)
    c_ancs_notif = svc->createCharacteristic(
        "6F726900-0010-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
    c_ancs_notif->setCallbacks(&s_char_cb);

    // 0011 ANCS Call State — Read + Notify, MITM-authenticated. Live
    // incoming/active call state, driven by ancs_client via
    // notify_ancs_call_state() below. READ required for WinRT notification
    // delivery, same as 000A/0010 above.
    c_ancs_call = svc->createCharacteristic(
        "6F726900-0011-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
    c_ancs_call->setCallbacks(&s_char_cb);

    // 0012 ANCS Notification Action — Write with response, MITM-authenticated.
    // { u: uid, a: 0=Positive/1=Negative } — Orion's remote Answer/Decline/
    // End call/Dismiss/Read-all. See handle_ancs_action() above.
    c_ancs_action = svc->createCharacteristic(
        "6F726900-0012-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_ancs_action->setCallbacks(&s_char_cb);

    // 0014 Firmware Update Control — Write with response + Notify, MITM.
    // Orion writes { o: "BEGIN"|"END"|"ABORT", ... }; Ori notifies READY /
    // REJECT / PROGRESS / RESUME / VALIDATED / FAILED back on the same char
    // (ble-protocol.md §14). READ is declared because it NOTIFIES — WinRT
    // silently drops notifications for a notify-only characteristic
    // (firmware.md); the read itself returns whatever status was sent last.
    c_fw_ctrl = svc->createCharacteristic(
        "6F726900-0014-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::READ  | NIMBLE_PROPERTY::READ_AUTHEN |
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN |
        NIMBLE_PROPERTY::NOTIFY);
    c_fw_ctrl->setCallbacks(&s_char_cb);

    // 0015 Firmware Update Data — Write-no-response for the stream, plus plain
    // Write so the sender can turn any frame into a windowed checkpoint whose
    // ATT ack proves the burst before it landed (same technique as §5's bulk
    // writes). Payload is [offset uint32 LE][image bytes] — not the §5 chunk
    // header: an absolute offset lets a dropped fragment cost one rewind
    // instead of the whole image, and has no uint16 fragment-count ceiling.
    c_fw_data = svc->createCharacteristic(
        "6F726900-0015-4F72-9F00-000000000000",
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::WRITE_AUTHEN);
    c_fw_data->setCallbacks(&s_char_cb);

    // In NimBLE 2.5, services are started when NimBLEServer::start() is called.
    // svc->start() is deprecated and a no-op; omit it.
    // The server is started in ble_manager::init() after all characteristics are set up.
    LOG("[gatt] Service registered: 21 characteristics + DIS Firmware Revision\n");
}

void announce_gatt_layout_change() {
    const uint8_t stored = nvs::get_gatt_layout();
    if (stored == ORI_GATT_LAYOUT_VERSION) return;

    // Whole-range indication (0x0001-0xFFFF) rather than the exact changed
    // span: the peer's correct response to either is "re-discover", and a
    // conservative range can't under-report a handle that moved.
    ble_svc_gatt_changed(0x0001, 0xFFFF);
    nvs::set_gatt_layout(ORI_GATT_LAYOUT_VERSION);
    LOG("[gatt] layout %u -> %u: Service Changed sent, peers will re-discover\n",
        (unsigned)stored, (unsigned)ORI_GATT_LAYOUT_VERSION);
}

bool is_ota_active() { return g_ota_active; }

void set_ota_active(bool active) {
    g_ota_active = active;
    LOG("[gatt] firmware update active: %d\n", (int)active);
}

// ── Firmware Update Control notifies (char 0014) ──────────────────────────
// One encoder for all six ops: { "o": <op> } plus at most one payload key.
// Also stored as the characteristic's readable value so a WinRT read (and the
// base READ property those notifies depend on) returns the last status.
static void notify_fw(const char* op, const char* reason,
                      const char* version, const uint32_t* bytes) {
    if (!c_fw_ctrl) return;
    CborEncoder enc, map;
    uint8_t buf[64];
    cbor_encoder_init(&enc, buf, sizeof(buf), 0);
    cbor_encoder_create_map(&enc, &map, (reason || version || bytes) ? 2 : 1);
    cbor_encode_text_stringz(&map, "o"); cbor_encode_text_stringz(&map, op);
    if (reason)      { cbor_encode_text_stringz(&map, "r"); cbor_encode_text_stringz(&map, reason); }
    else if (version){ cbor_encode_text_stringz(&map, "v"); cbor_encode_text_stringz(&map, version); }
    else if (bytes)  { cbor_encode_text_stringz(&map, "b"); cbor_encode_uint(&map, *bytes); }
    cbor_encoder_close_container(&enc, &map);
    size_t n = cbor_encoder_get_buffer_size(&enc, buf);
    c_fw_ctrl->setValue(buf, n);
    c_fw_ctrl->notify();
}

void notify_fw_status(const char* op) { notify_fw(op, nullptr, nullptr, nullptr); }

void notify_fw_reason(const char* op, const char* reason) {
    LOG("[gatt] fw %s: %s\n", op, reason);
    notify_fw(op, reason, nullptr, nullptr);
}

void notify_fw_bytes(const char* op, uint32_t bytes) {
    notify_fw(op, nullptr, nullptr, &bytes);
}

void notify_fw_validated(const char* version) {
    notify_fw("VALIDATED", nullptr, version, nullptr);
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
    // Remember our own vol_set value so handle_host_volume() can recognize
    // Orion's direct confirmation of it (see g_last_vol_set_value's comment).
    if (strcmp(op, "vol_set") == 0) {
        g_last_vol_set_value = (uint8_t)(arg > 100 ? 100 : arg);
    }
    // Encode KeyboardCommand { op, arg }
    CborEncoder enc, map;
    uint8_t buf[64];
    cbor_encoder_init(&enc, buf, sizeof(buf), 0);
    cbor_encoder_create_map(&enc, &map, 2);
    cbor_encode_text_stringz(&map, "o");
    cbor_encode_text_stringz(&map, op);
    cbor_encode_text_stringz(&map, "a");
    cbor_encode_uint(&map, arg);
    cbor_encoder_close_container(&enc, &map);
    size_t n = cbor_encoder_get_buffer_size(&enc, buf);
    c_kbd_cmd->notify(buf, n);
    LOG("[gatt] KeyboardCommand: op=%s arg=%u\n", op, (unsigned)arg);
}

void notify_phone_bond_status(bool bonded, bool connected, const char* name,
                               const char* device_type, uint8_t battery_level) {
    if (!c_phone_status) return;
    g_phone_bonded    = bonded;
    g_phone_connected = connected;
    strncpy(g_phone_name_cache, name ? name : "", sizeof(g_phone_name_cache) - 1);
    g_phone_name_cache[sizeof(g_phone_name_cache) - 1] = '\0';
    strncpy(g_phone_device_type_cache, device_type ? device_type : "",
            sizeof(g_phone_device_type_cache) - 1);
    g_phone_device_type_cache[sizeof(g_phone_device_type_cache) - 1] = '\0';
    if (!connected) {
        // Nothing left to verify once the link drops — zero the stats/signal/
        // battery cache too so a stale reading never lingers into the next
        // connect (ancs_client re-populates real values once ANCS resubscribes).
        g_phone_missed = g_phone_unread = g_phone_total = g_phone_signal = g_phone_battery = 0;
    } else {
        // Fresh (re)connect: trust the caller's just-read battery level over
        // this characteristic's own cache, which the branch above zeroed on
        // the prior disconnect.
        g_phone_battery = battery_level;
    }
    uint8_t buf[192];
    size_t  n = encode_phone_status(buf, sizeof(buf), bonded, connected, name, device_type,
                                     g_phone_missed, g_phone_unread, g_phone_total,
                                     g_phone_signal, g_phone_battery);
    c_phone_status->setValue(buf, n);
    c_phone_status->notify(buf, n);
    LOG("[gatt] PhoneBondStatus: bonded=%d connected=%d name='%s' type='%s'\n",
        (int)bonded, (int)connected, name ? name : "", device_type ? device_type : "");
}

void notify_phone_stats(uint8_t missed, uint8_t unread, uint8_t total,
                         uint8_t signal_bars, uint8_t battery_level) {
    // Nothing to relay while disconnected — notify_phone_bond_status() already
    // zeroed and pushed the cache when the link dropped.
    if (!c_phone_status || !g_phone_connected) return;
    if (missed == g_phone_missed && unread == g_phone_unread &&
        total == g_phone_total && signal_bars == g_phone_signal &&
        battery_level == g_phone_battery) {
        return;  // no real change — avoid spamming a notify per ANCS event
    }
    g_phone_missed  = missed;
    g_phone_unread  = unread;
    g_phone_total   = total;
    g_phone_signal  = signal_bars;
    g_phone_battery = battery_level;
    uint8_t buf[192];
    size_t  n = encode_phone_status(buf, sizeof(buf), g_phone_bonded, g_phone_connected,
                                     g_phone_name_cache, g_phone_device_type_cache,
                                     g_phone_missed, g_phone_unread, g_phone_total,
                                     g_phone_signal, g_phone_battery);
    c_phone_status->setValue(buf, n);
    c_phone_status->notify(buf, n);
    LOG("[gatt] PhoneBondStatus stats: missed=%u unread=%u total=%u signal=%u battery=%u\n",
        (unsigned)missed, (unsigned)unread, (unsigned)total, (unsigned)signal_bars,
        (unsigned)battery_level);
}

// -- ANCS relay to Orion (chars 0010-0012, ble-protocol.md §13) ------------
// All four notify_ancs_* functions are called from ancs_client.cpp, which
// owns the filter gate and the call/non-call routing decision (§13's "Filter
// gates the relay, not just the display" / the scoping note that calls never
// touch char 0010) — these just encode + notify exactly what they're given.

// Send-side chunking for char 0010 (ble-protocol.md §5's "AncsNotification
// chunking") — the only notify characteristic in this file whose payload can
// exceed one ATT notification, now that AncsNotification's field caps match
// Ori's own on-device storage (cbor_encode_ancs_add's doc comment) instead of
// being squeezed to fit a single fragment. Reuses §5's exact frame format
// (seq_num/total_frags/payload_len, uint16 LE) in the REVERSE direction —
// Ori is the sender here, Orion the reassembler (central.rs). ALWAYS used for
// every op (add/remove/clear), even though remove/clear always fit in one
// frame (total_frags=1): a single always-framed format means Orion's
// reassembler never has to guess whether a given notify is a raw payload or
// a chunk frame.
//
// Frame size comes from the actual negotiated MTU for the Orion connection —
// same adaptive approach central.rs's frag_size_for_mtu already uses for the
// write direction — clamped to the existing ~238-byte convention so this
// doesn't silently grow past what the rest of the protocol assumes.
//
// Sent as a tight synchronous loop: GATT server callbacks run on Ori's single
// main task, so nothing else can interleave another notify on this same
// characteristic mid-sequence — Orion's reassembler only ever has one
// fragment sequence in flight at a time, never interleaved across two
// different notifications.

// Bounded retry-with-backoff for a single notify() call, scoped to the
// transient "outgoing mbuf pool momentarily full" case — NOT a general
// reliability mechanism and NOT a change to §5's "no NACK/retry" protocol
// design for a single notification (that reasoning is still correct: a lone
// AncsNotification's few fragments comfortably fit the pool). The one case
// that DOES overrun the pool is ancs_client.cpp's resync_orion_relay()
// stacking many queued notifications' worth of fragments back-to-back right
// at reconnect — a real hardware log showed 10 queued notifications (several
// up to 8 fragments each) flooding the link while still mid-MTU-negotiation,
// most fragments failing and Orion eventually tearing down the connection
// (BLE_HS_HCI_ERR 0x13, "Remote User Terminated Connection"). The primary fix
// for that is pacing the resync loop itself (ancs_client.cpp); this retry is
// defense in depth for whatever transient overrun still slips through.
//
// NimBLECharacteristic::notify() only returns bool — the actual ble_hs rc
// (e.g. BLE_HS_ENOMEM) is logged inside NimBLE's own sendValue() under its
// own log tag and never surfaced to the caller, so we can't branch on
// ENOMEM specifically. In practice, on an already-subscribed/encrypted
// characteristic the near-only realistic failure mode IS the mbuf pool
// being transiently full, so retrying on any notify() failure here is the
// closest available proxy.
static constexpr int      ANCS_NOTIFY_MAX_RETRIES        = 3;
static constexpr uint32_t ANCS_NOTIFY_RETRY_BACKOFF_MS    = 15;

static bool notify_with_retry(NimBLECharacteristic* c, const uint8_t* frame, size_t len) {
    bool ok = c->notify(frame, len);
    for (int attempt = 0; !ok && attempt < ANCS_NOTIFY_MAX_RETRIES; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(ANCS_NOTIFY_RETRY_BACKOFF_MS));
        ok = c->notify(frame, len);
    }
    return ok;
}

static void notify_chunked(NimBLECharacteristic* c, const uint8_t* payload, size_t payload_len) {
    if (!c) return;
    uint16_t mtu = ble_att_mtu(ble_manager::orion_conn_handle());
    // Usable payload per fragment = ATT_MTU - 3 (ATT header) - 6 (frame
    // header), same accounting as §5's 247-byte-MTU example. This used to be
    // floored to a flat 20 bytes "for pathological small MTUs," but the one
    // small MTU this protocol actually documents — §5's fallback of 23, used
    // when the peer refuses 247 — computes to 14 bytes here, and that floor
    // pushed it up to 20 anyway: a 6+20=26-byte frame written into a
    // connection whose real ATT payload budget (mtu-3) is only 20 bytes.
    // Every AncsNotification relayed on an MTU-23 connection was 6 bytes
    // oversized and would be rejected or truncated by the BLE stack. Never
    // round frag_size UP past what the negotiated MTU can actually carry —
    // only clamp the upper bound (238, this file's existing wire convention)
    // and guard the degenerate near-zero-MTU case.
    size_t frag_size = (mtu > 9) ? (size_t)(mtu - 3 - 6) : 14;
    if (frag_size > 238) frag_size = 238;
    if (frag_size < 1)   frag_size = 1;  // pathological floor — never zero

    size_t total_frags = payload_len ? (payload_len + frag_size - 1) / frag_size : 1;
    uint8_t frame[6 + 238];
    for (size_t seq = 0; seq < total_frags; ++seq) {
        size_t offset   = seq * frag_size;
        size_t this_len = payload_len - offset;
        if (this_len > frag_size) this_len = frag_size;
        frame[0] = (uint8_t)(seq & 0xFF);
        frame[1] = (uint8_t)((seq >> 8) & 0xFF);
        frame[2] = (uint8_t)(total_frags & 0xFF);
        frame[3] = (uint8_t)((total_frags >> 8) & 0xFF);
        frame[4] = (uint8_t)(this_len & 0xFF);
        frame[5] = (uint8_t)((this_len >> 8) & 0xFF);
        memcpy(frame + 6, payload + offset, this_len);
        if (!notify_with_retry(c, frame, 6 + this_len)) {
            LOG("[gatt] notify_chunked: frame %u/%u FAILED after %d retries (mbuf pool exhausted?)\n",
                (unsigned)(seq + 1), (unsigned)total_frags, ANCS_NOTIFY_MAX_RETRIES);
        }
    }
}

void notify_ancs_add(uint32_t uid, const char* icon_token, uint8_t category,
                      const char* app, const char* title, const char* body,
                      uint32_t recv_epoch, const char* pos_label,
                      const char* neg_label, bool has_neg_action, bool silent) {
    if (!c_ancs_notif) return;
    // Worst case: token(31)+display_name(39)+title(192)+body(512)+
    // pos_label(32)+neg_label(32) all near max, plus CBOR/map overhead —
    // 893 bytes; 1024 gives comfortable headroom (matches g_ds_buf's own
    // 1024-byte convention in ancs_client.cpp for "the ANCS max buffer size").
    uint8_t buf[1024];
    size_t  n = cbor_encode_ancs_add(buf, sizeof(buf), uid, icon_token, category,
                                      app, title, body, recv_epoch, pos_label,
                                      neg_label, has_neg_action, silent);
    notify_chunked(c_ancs_notif, buf, n);
    LOG("[gatt] AncsNotification add: uid=%u app=%s cat=%u len=%u\n",
        (unsigned)uid, app ? app : "", (unsigned)category, (unsigned)n);
}

void notify_ancs_remove(uint32_t uid) {
    if (!c_ancs_notif) return;
    uint8_t buf[32];
    size_t  n = cbor_encode_ancs_remove(buf, sizeof(buf), uid);
    notify_chunked(c_ancs_notif, buf, n);
    LOG("[gatt] AncsNotification remove: uid=%u\n", (unsigned)uid);
}

void notify_ancs_clear() {
    if (!c_ancs_notif) return;
    uint8_t buf[16];
    size_t  n = cbor_encode_ancs_clear(buf, sizeof(buf));
    notify_chunked(c_ancs_notif, buf, n);
    LOG("[gatt] AncsNotification clear\n");
}

void notify_ancs_call_state(uint8_t st, uint32_t uid, uint32_t elapsed_s,
                             const char* app, const char* title,
                             const char* pos_label, const char* neg_label,
                             bool has_neg_action, const char* icon_token) {
    if (!c_ancs_call) return;
    // char 0011 is NOT chunked and keeps its own independent, tighter field
    // caps (app/title/pos/neg/icon_token, cbor_encode_ancs_call_state) —
    // calls carry no body, so the single-fragment squeeze that motivated
    // matching AncsNotification's caps to Ori's storage doesn't apply here
    // (ble-protocol.md §10). This buffer only needs to be big enough for
    // those tighter caps, unrelated to AncsNotification's own (much larger)
    // buffer size.
    uint8_t buf[192];
    size_t  n = cbor_encode_ancs_call_state(buf, sizeof(buf), st, uid, elapsed_s,
                                             app, title, pos_label, neg_label,
                                             has_neg_action, icon_token);
    c_ancs_call->notify(buf, n);
    LOG("[gatt] AncsCallState: st=%u uid=%u elapsed=%u title=%s\n",
        (unsigned)st, (unsigned)uid, (unsigned)elapsed_s, title ? title : "");
}

void nack_sync_control(const char* reason) {
    if (!c_sync_ctrl) return;
    uint8_t buf[64];
    size_t  n = cbor_encode_sync_ctrl(buf, sizeof(buf), "NACK", g_sync_seq, reason);
    c_sync_ctrl->notify(buf, n);
    LOG("[gatt] SyncControl NACK: %s\n", reason ? reason : "");
}

void abort_sync_stage() {
    // Reset any in-flight chunk reassembly unconditionally — g_art_ctx (Media
    // Album Art) lives outside the BEGIN/END staging pipeline (ble-protocol.md
    // §12), so it can be mid-transfer even when g_stage/g_sync_in_progress are
    // both false. Without this, a disconnect mid-fragment leaks that context's
    // PSRAM buffer permanently (chunked_transfer::reset() is a no-op on an
    // already-idle context, so this is always safe to call).
    chunked_transfer::reset(&g_photo_ctx);
    chunked_transfer::reset(&g_meetings_ctx);
    chunked_transfer::reset(&g_time_off_ctx);
    chunked_transfer::reset(&g_art_ctx);
    chunked_transfer::reset(&g_lunar_ctx);
    chunked_transfer::reset(&g_ancs_apps_ctx);

    if (!g_stage.active && !g_sync_in_progress) return;
    LOG("[gatt] Sync aborted (disconnect) — discarding staged data\n");
    g_sync_in_progress = false;
    stage_reset();
}

// Poll all chunk-reassembly contexts for the 10 s no-progress timeout
// (ble-protocol.md §5 NACK_CHUNK_TIMEOUT) — catches a sender that stalls
// mid-transfer without disconnecting (abort_sync_stage() above only covers
// the disconnect case). Call once per second from ble_manager::poll().
void poll_chunk_timeouts() {
    chunked_transfer::Context* ctxs[] = {
        &g_photo_ctx, &g_meetings_ctx, &g_time_off_ctx, &g_art_ctx, &g_lunar_ctx,
        &g_ancs_apps_ctx,
    };
    chunked_transfer::poll_timeouts(ctxs, 6);
}

// Notifies Device Settings (char 000E) whenever Ori's own live signal_bars
// ("r") to Orion changes (ble-protocol.md §4/§6.4, pc-app.md) — the actual
// work lives on OriCharacteristicCallbacks::poll_orion_signal_bars() (public
// member, near onSubscribe() above), since it needs read_orion_signal_bars()/
// handle_device_settings_read()/the last-notified cache, all private members
// of that class. This is just the public forwarding entry point
// ble_manager::poll() calls every tick.
void poll_orion_signal_bars() {
    s_char_cb.poll_orion_signal_bars();
}

// Drains the alert-settings latch (g_alert_latch, near DeviceSettingsWrite
// above) onto NVS — one nvs::set_*() per pending field. Call once per tick
// from ble_manager::poll() (main task); see gatt_server.h's doc comment for
// why this exists instead of six more BleEventType entries.
void poll_alert_settings() {
    if (g_alert_latch.work_hours_end_pending) {
        g_alert_latch.work_hours_end_pending = false;
        nvs::set_work_hours_end_min(g_alert_latch.work_hours_end_val);
    }
    if (g_alert_latch.work_hours_days_pending) {
        g_alert_latch.work_hours_days_pending = false;
        nvs::set_work_hours_days(g_alert_latch.work_hours_days_val);
    }
    if (g_alert_latch.weather_alert_en_pending) {
        g_alert_latch.weather_alert_en_pending = false;
        nvs::set_weather_alert_enabled(g_alert_latch.weather_alert_en_val);
    }
    if (g_alert_latch.weather_alert_offset_pending) {
        g_alert_latch.weather_alert_offset_pending = false;
        nvs::set_weather_alert_offset_min(g_alert_latch.weather_alert_offset_val);
    }
    if (g_alert_latch.low_batt_alert_en_pending) {
        g_alert_latch.low_batt_alert_en_pending = false;
        nvs::set_low_battery_alert_enabled(g_alert_latch.low_batt_alert_en_val);
    }
    if (g_alert_latch.low_batt_threshold_pending) {
        g_alert_latch.low_batt_threshold_pending = false;
        nvs::set_low_battery_threshold_pct(g_alert_latch.low_batt_threshold_val);
    }
}

// Called from ble_manager::poll() (main task) in response to
// SyncControl{op:"END"}. Applies all staged items to NVS/UI in one burst,
// then transitions Device Status and signals SyncEnd for the UI advance.
void run_staged_commit() {
    // Only blank the display ahead of items that actually hit flash: Profile,
    // Photo, and Time Off call into nvs_sync::save_*(). Time, Meetings, and
    // Shortcuts are all RAM-only — no flash write, so no glitch window to
    // guard against. Read g_stage here, before stage_commit() resets it.
    bool needs_nvs = g_stage.have_profile || g_stage.have_photo || g_stage.have_time_off;
    if (needs_nvs) {
        // Blank the display immediately before the flash write burst. LCD_CAM
        // DMA keeps scanning the (now black) framebuffer, so there are no
        // rendering glitches during NVS / LittleFS writes.
        lcd_panel::blackout();
        // Tell state_machine::on_reconnect_end() (fired below via
        // ble_post_sync_end_event()) that this sync needs an explicit
        // repaint once it's done, regardless of whether this sync was big
        // enough to have shown the reconnect-syncing overlay — see
        // mark_display_needs_repaint()'s own doc comment (state_machine.h)
        // for the bug this closes (a small profile/photo-only edit blanked
        // the screen here but, before this flag existed, nothing repainted
        // it afterward: the status bar, mode-toggle, and meeting list all
        // stayed empty even though their widgets were untouched).
        state_machine::mark_display_needs_repaint();
    }

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

// -- Volume swipe state setters (called from screen_media_mode BLE hooks) ---

extern "C" {
void gatt_server_set_vol_swipe_active(bool active) {
    g_vol_swipe_active = active;
    if (!active) {
        g_vol_swipe_end_ms = (uint32_t)millis();
    }
}
}
