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
//
// Concurrency / timing model (this is the part that must not regress):
//   - poll() runs on the Arduino loopTask, the same task as lv_timer_handler().
//   - poll() is COOPERATIVELY BOUNDED: it processes at most POLL_BUDGET_MS of
//     serial input per call, then returns so loop() can service LVGL and the
//     idle task can run (otherwise the Task WDT reboots us mid-update).
//   - DATA payload is consumed in BULK (Serial.read(buf, n) + one Update.write
//     + one sha256_update per chunk), never byte-at-a-time.
//   - While a transfer owns the port (is_busy()/is_active()), the debug serial
//     consumer (screen_manager::poll_serial) is gated off so it cannot steal
//     OTA bytes when poll() returns mid-frame on its time budget.
//   - A stall watchdog aborts the update if no DATA arrives for
//     OTA_STALL_TIMEOUT_MS (USB unplugged / host hang) and resumes runtime.

#include "ota_receiver.h"

#include <Arduino.h>
#include "ori_log.h"
#include <Update.h>
#include <mbedtls/sha256.h>
#include <esp_heap_caps.h>
#include <string.h>

#include <cbor.h>

#include "fw_version.h"
#include "lcd_panel.h"
#include "nvs_store.h"
#include "state_machine.h"
#include "ble/ble_manager.h"
#include "ble/gatt_server.h"
#include "screens/screen_ota_updating.h"
#include <lvgl.h>

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

// Progress notification cadence. PROGRESS doubles as the sender's flow-control
// ack, so it must fire often enough (in bytes) that the host can keep a tight
// window and never overrun the RX buffer — see Serial.setRxBufferSize() in
// main.cpp and the windowed sender in tools/mock_orion_ota.py.
#define PROGRESS_INTERVAL_PCT 5
#define PROGRESS_MAX_BYTES    8192

// Cooperative limits.
//   POLL_BUDGET_MS    — max wall time spent draining serial per poll() call.
//                       Keeps the loop responsive (LVGL render + idle task feed).
//                       One DATA chunk can trigger a 4 KB flash sector erase
//                       (~30-40 ms), so a single poll may overshoot by one
//                       sector write — still far under the ~5 s Task WDT.
//   OTA_STALL_TIMEOUT — abort if no DATA progress for this long (matches the
//                       10 s chunk timeout in ble-protocol.md §5).
static const uint32_t POLL_BUDGET_MS      = 30;
// Silence on the wire mid-download = broken cable / host hang. During a healthy
// transfer DATA is near-continuous (sub-100 ms gaps), so 3 s is ~30x the largest
// normal gap — fast to react, with margin for OS-scheduling hiccups on the PC.
// (The 10 s figure in ota.md is the BLE-chunking timeout, not this USB path.)
static const uint32_t OTA_STALL_TIMEOUT_MS = 3000;

// How long the "Installing… screen goes dark" frame stays up before the flash
// commit blanks the panel — long enough for the user to read it (and the
// countdown bar to run).
static const uint32_t COMMIT_LINGER_MS    = 3500;

// Bulk DATA scratch buffer. 1 KB keeps per-read latency low while amortising
// Serial.read() call overhead; a chunk never spans more than one sector flush.
static const uint32_t RX_CHUNK = 1024;

// Embedded firmware-version marker. Compiled into every Ori image so the OTA
// receiver can read the *incoming* image's own version straight from the staged
// binary (authoritative) instead of trusting the fw_version string Orion sends
// in BEGIN. The stock esp_app_desc.version on this precompiled Arduino core is
// its git hash ("arduino-lib-builder"), not ours — so we stamp our own.
//
// It MUST be a single contiguous char[] (one string literal): a multi-field
// struct gets split/reordered by the optimizer (-O2 ICF), scattering the
// members. The scanner (extract_image_version) finds the "OriFwVer=" prefix and
// reads the null-terminated version after it.
extern "C" __attribute__((used))
const char g_ori_fw_marker[] = "OriFwVer=" ORI_FW_VERSION;

// ── State machine ──────────────────────────────────────────────────────────

// OTA lifecycle. is_active() (sticky OTA screen + BLE NACK gate) is true for any
// state other than Idle.
//   Idle         — no transfer
//   AwaitingData — streaming image into PSRAM (Updating screen, live ring)
//   Installing   — image received + verified; "Installing firmware" frame up,
//                  counting down COMMIT_LINGER_MS before the flash commit
//   Failed       — "Update failed" screen up, waiting for the user to tap Close
enum class OtaState : uint8_t {
    Idle,
    AwaitingData,
    Installing,
    Failed,
};

namespace {

OtaState g_ota_state  = OtaState::Idle;

// SHA-256 context (streaming) + whether it's currently initialised (guards
// against a double-free on the failure paths).
mbedtls_sha256_context g_sha256_ctx;
bool     g_sha_active     = false;
uint8_t  g_expected_sha256[32] = {};
uint32_t g_total_size     = 0;
uint32_t g_received_bytes = 0;
uint32_t g_next_progress  = 0;  // byte threshold for the next PROGRESS frame
uint32_t g_progress_step  = 1;  // bytes per PROGRESS_INTERVAL_PCT (>=1)
uint32_t g_last_rx_ms     = 0;  // millis() of the last DATA byte (stall watchdog)
char     g_claimed_version[24] = {}; // fw_version Orion declared at BEGIN (the claim)
char     g_new_version[24]    = {};  // version read from the binary — shown on the ack

// Frame parser cursor.
uint32_t g_payload_len  = 0;
uint32_t g_payload_read = 0;
uint8_t  g_current_op   = 0;

// PSRAM-staging model (keeps the progress bar live):
//
// The RGB panel has no frame memory and scans its framebuffer out of PSRAM
// continuously; PSRAM reads and flash writes contend on the shared MSPI bus, so
// the display and flash writes can't run at the same instant. Instead of
// writing flash during the download (which would force the screen dark the whole
// time), we stream the whole image into a PSRAM buffer first — no flash writes,
// so the LCD keeps refreshing and the progress bar advances live. Only at END,
// once the image is received and its hash checked, do we halt the LCD and burst
// the staged image to flash (a few seconds dark), then reboot.
//
// Failures during the download phase (truncated / hash / overflow / stall) free
// the buffer and resume runtime with the display still alive — no reboot. Only
// the successful flash commit halts the LCD and reboots.
uint8_t* g_stage_buf     = nullptr;  // PSRAM staging buffer (g_total_size bytes)
bool     g_commit_pending = false;   // END ok → flash once g_commit_at passes
uint32_t g_commit_at      = 0;       // millis() to start the commit (lets the
                                     // "Installing…" frame render + linger)

// ── Output helpers ─────────────────────────────────────────────────────────

static void send_frame(uint8_t op, const uint8_t* payload, uint32_t payload_len) {
    // Emit header+payload in ONE Serial.write so a concurrent LOG() from the
    // NimBLE task (core 0) on this shared USB CDC port can't interleave between
    // the header and payload and corrupt the frame. All OTA response payloads are
    // tiny (≤64 B); the oversized branch is a safety net that never fires here.
    uint8_t frame[6 + 128];
    frame[0] = OTA_MAGIC_0;
    frame[1] = OTA_MAGIC_1;
    frame[2] = op;
    frame[3] = (uint8_t)(payload_len & 0xFF);
    frame[4] = (uint8_t)((payload_len >> 8) & 0xFF);
    frame[5] = (uint8_t)((payload_len >> 16) & 0xFF);
    if (payload_len <= sizeof(frame) - 6) {
        if (payload && payload_len > 0) memcpy(frame + 6, payload, payload_len);
        Serial.write(frame, 6 + payload_len);
    } else {
        Serial.write(frame, 6);
        if (payload && payload_len > 0) Serial.write(payload, payload_len);
    }
    Serial.flush();
}

static void send_empty_response(uint8_t op) {
    uint8_t empty_map = 0xA0;  // CBOR {}
    send_frame(op, &empty_map, 1);
}

// REJECT and FAILED carry the same { "reason": <text> } CBOR payload — only the
// op differs. Encode once.
static void send_reason(uint8_t op, const char* reason) {
    CborEncoder enc, map;
    uint8_t buf[64];
    cbor_encoder_init(&enc, buf, sizeof(buf), 0);
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_text_stringz(&map, "reason");
    cbor_encode_text_stringz(&map, reason);
    cbor_encoder_close_container(&enc, &map);
    send_frame(op, buf, (uint32_t)cbor_encoder_get_buffer_size(&enc, buf));
}

static inline void send_reject(const char* reason) { send_reason(OTA_OP_REJECT, reason); }
static inline void send_failed(const char* reason) { send_reason(OTA_OP_FAILED, reason); }

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

// ── Frame parser ───────────────────────────────────────────────────────────
// Header + control payloads are parsed byte-at-a-time (small, infrequent).
// DATA payload is consumed in bulk by poll() — parse_byte never sees DATA bytes.

enum class ParseState : uint8_t {
    WaitMagic0,
    WaitMagic1,
    WaitOp,
    WaitLen0,
    WaitLen1,
    WaitLen2,
    ReadPayload,      // control-frame payload (BEGIN) — buffered
    ReadDataStream,   // DATA payload — bulk-consumed by poll(), not parse_byte()
};

ParseState g_parse = ParseState::WaitMagic0;

static uint8_t  g_ctrl_buf[256];
static uint32_t g_ctrl_pos = 0;

static void free_stage() {
    if (g_stage_buf) {
        heap_caps_free(g_stage_buf);
        g_stage_buf = nullptr;
    }
}

// Scan a staged image for the embedded version marker ("OriFwVer=<version>") and
// copy the version into `out` (null-terminated). Returns false if not found
// (e.g. an older image built before the marker existed). Linear byte scan with a
// first-byte pre-filter — a few ms over a 1.6 MB image.
//
// The 9-byte search pattern is stored XOR-obfuscated and rebuilt on the stack so
// it never sits verbatim in our own rodata — otherwise the scanner's own copy of
// the pattern would be a second false match (and could be constant-merged).
static bool extract_image_version(const uint8_t* img, uint32_t len,
                                  char* out, size_t out_sz) {
    static const uint8_t OBF[9] = {                       // "OriFwVer=" ^ 0x5A
        'O'^0x5A, 'r'^0x5A, 'i'^0x5A, 'F'^0x5A, 'w'^0x5A,
        'V'^0x5A, 'e'^0x5A, 'r'^0x5A, '='^0x5A };
    uint8_t magic[9];
    for (int k = 0; k < 9; k++) magic[k] = OBF[k] ^ 0x5A;
    if (len < 9) return false;
    for (uint32_t i = 0; i + 9 < len; i++) {
        if (img[i] != magic[0]) continue;                 // pre-filter
        if (memcmp(img + i, magic, 9) != 0) continue;
        const char* v   = (const char*)(img + i + 9);
        uint32_t    cap = len - (i + 9);
        size_t n = 0;
        while (n < 23 && n + 1 < out_sz && n < cap && v[n]) { out[n] = v[n]; n++; }
        out[n] = '\0';
        return n > 0;
    }
    return false;
}

// ── Failure → friendly message + error screen ───────────────────────────────

// Map a protocol failure code to a plain-language line for the Update-failed
// screen. (The wire still carries the terse code in the REJECT/FAILED frame.)
// Table-driven — same code -> message mapping as before, just not a chain of
// separately-typed-out strcmp/return pairs; unmatched codes still fall through
// to the same generic line.
static const char* friendly_reason(const char* code) {
    static const struct { const char* code; const char* message; } kReasons[] = {
        { "usb_timeout",      "Lost connection — the USB cable may have come loose. Try again" },
        { "truncated",        "The download didn't finish — check the cable and try again" },
        { "hash_mismatch",    "The update was corrupted in transfer (checksum failed)" },
        { "size_overflow",    "The update package was invalid (size mismatch)" },
        { "flash_error",      "Couldn't save the update to storage. Try again" },
        { "version_mismatch", "The update didn't match its expected version — try again from Orion" },
        { "bad_image",        "That file isn't a valid Ori firmware image" },
        { "too_large",        "This update is too large for Ori" },
        { "no_memory",        "Not enough memory to receive the update. Try again" },
    };
    for (const auto& r : kReasons) {
        if (!strcmp(code, r.code)) return r.message;
    }
    return "The update couldn't be completed. Please try again";
}

// LVGL button callback (runs on the loop task inside lv_timer_handler).
// dismiss_error() is the public API declared in the header.
static void btn_dismiss_cb(lv_event_t*) { ota_receiver::dismiss_error(); }

// Show the "Update failed" screen and park in Failed until the user taps Close.
// The terminal REJECT/FAILED frame must already have been sent.
static void show_error_screen(const char* code) {
    free_stage();
    gatt_server::set_ota_active(false);
    // Lift OTA quiet mode: re-arm advertising, resume ANCS processing, and —
    // if any ANCS event was dropped during the download — force the iPhone
    // reconnect that replays them. Safe no-op for pre-accept rejects
    // (too_large / no_memory), where quiet mode was never entered.
    ble_manager::set_ota_transfer_quiet(false);
    g_ota_state = OtaState::Failed;
    g_parse     = ParseState::WaitMagic0;
    while (Serial.available() > 0) Serial.read();   // drop any in-flight bytes
    state_machine::ota_show(
        screen_ota_updating::create_error(friendly_reason(code), btn_dismiss_cb));
    LOG("[ota] error: %s\n", code);
}

// Download-phase failure: send FAILED + show the error screen (LCD still live).
static void fail_in_ota(const char* code) {
    if (g_sha_active) { mbedtls_sha256_free(&g_sha256_ctx); g_sha_active = false; }
    send_failed(code);
    show_error_screen(code);
}

// Fields parsed out of a BEGIN frame's CBOR payload — see parse_begin_frame().
struct BeginFrame {
    char     fw_version[32];
    uint64_t total_size;
    uint8_t  sha256[32];
};

// Parses and validates a BEGIN frame's CBOR payload: { fw_version, total_size,
// sha256 }. On success, fills `out` and returns true. On failure, sends the
// matching REJECT frame itself (distinguishing malformed CBOR — "cbor_decode" /
// "not_map" — from a structurally valid map that's simply missing a required
// field — "missing_fields") and returns false; the caller just checks the
// return value and stops.
static bool parse_begin_frame(const uint8_t* payload, uint32_t len, BeginFrame& out) {
    CborParser parser;
    CborValue  root, map_val;
    if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) {
        send_reject("cbor_decode");
        return false;
    }
    if (!cbor_value_is_map(&root)) {
        send_reject("not_map");
        return false;
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

    // fw_version is required: the BEGIN claim is one half of the version
    // consistency check (the binary's embedded version is the other — see
    // handle_end). Without it there's nothing to verify the image against.
    if (!got_size || !got_sha || !got_version) {
        send_reject("missing_fields");
        return false;
    }

    strncpy(out.fw_version, recv_version, sizeof(out.fw_version) - 1);
    out.fw_version[sizeof(out.fw_version) - 1] = '\0';
    out.total_size = total_size;
    memcpy(out.sha256, sha256, 32);
    return true;
}

static void handle_begin(const uint8_t* payload, uint32_t len) {
    // Reject a second BEGIN while a transfer is already in progress — never
    // restart mid-flow. (No error screen: don't disrupt the active OTA.)
    if (g_ota_state != OtaState::Idle) {
        send_reject("busy");
        return;
    }

    BeginFrame begin;
    if (!parse_begin_frame(payload, len, begin)) return;

    // No countdown guard: the OTA is user-initiated in Orion, so the user's
    // explicit intent overrides the 5-minute pre-meeting alert. Once accepted,
    // is_active() makes OTA_UPDATING the top-priority state (state-machine.md), so
    // it takes over the countdown modal, and the alert is suppressed while active.

    // These rejects ARE user-relevant — surface them on the Update-failed screen
    // (the wire still gets the REJECT frame so Orion knows too).
    const uint32_t OTA_SLOT_SIZE = 3 * 1024 * 1024;
    if (begin.total_size == 0 || begin.total_size > OTA_SLOT_SIZE) {
        send_reject("too_large");
        show_error_screen("too_large");
        return;
    }

    // No version decision here — we can't read the image's embedded version
    // until it's downloaded. handle_end compares this BEGIN claim against the
    // version stamped inside the binary and rejects only if they DISAGREE.

    // Stage the whole image in PSRAM. No flash is touched during the download,
    // so the LCD keeps running and the progress bar stays live. Flash is written
    // in one burst at END (see do_commit).
    free_stage();
    g_stage_buf = (uint8_t*)heap_caps_malloc((size_t)begin.total_size, MALLOC_CAP_SPIRAM);
    if (!g_stage_buf) {
        LOG("[ota] PSRAM staging alloc failed (%u bytes)\n", (unsigned)begin.total_size);
        send_reject("no_memory");
        show_error_screen("no_memory");
        return;
    }

    g_total_size     = (uint32_t)begin.total_size;
    g_received_bytes = 0;
    g_progress_step  = g_total_size / (100 / PROGRESS_INTERVAL_PCT);
    if (g_progress_step == 0) g_progress_step = 1;       // tiny image guard
    // Cap the PROGRESS cadence so acks are frequent enough for the sender's
    // windowed flow control (it must not get more than a buffer-width ahead).
    if (g_progress_step > PROGRESS_MAX_BYTES) g_progress_step = PROGRESS_MAX_BYTES;
    g_next_progress  = g_progress_step;
    g_last_rx_ms     = millis();
    memcpy(g_expected_sha256, begin.sha256, 32);
    strncpy(g_claimed_version, begin.fw_version, sizeof(g_claimed_version) - 1);
    g_claimed_version[sizeof(g_claimed_version) - 1] = '\0';

    mbedtls_sha256_init(&g_sha256_ctx);
    mbedtls_sha256_starts(&g_sha256_ctx, 0);
    g_sha_active  = true;

    g_ota_state   = OtaState::AwaitingData;

    gatt_server::set_ota_active(true);   // NACK BLE data writes for the duration
    // Stop advertising + suspend ANCS processing for the whole download — no
    // reconnect ceremony or notification storm can compete with the USB CDC
    // RX path (ble_manager.h's set_ota_transfer_quiet doc comment). Reversed
    // by show_error_screen() on any failure; a successful commit reboots.
    ble_manager::set_ota_transfer_quiet(true);
    state_machine::on_ota_begin();       // full-screen OTA takeover (Updating)

    send_empty_response(OTA_OP_READY);
    LOG("[ota] BEGIN accepted: size=%u ver=%s\n",
        (unsigned)begin.total_size, begin.fw_version);
}

// Bulk DATA handler — copies into the PSRAM staging buffer (no flash write, so
// the display stays live). Returns false (and has already aborted) on error.
static bool feed_data(const uint8_t* buf, uint32_t n) {
    if (g_ota_state != OtaState::AwaitingData || !g_stage_buf) return false;
    if (n == 0) return true;

    // Host overran the declared size — protocol error.
    if (n > g_total_size - g_received_bytes) {
        fail_in_ota("size_overflow");
        return false;
    }

    memcpy(g_stage_buf + g_received_bytes, buf, n);
    mbedtls_sha256_update(&g_sha256_ctx, buf, n);
    g_received_bytes += n;

    if (g_received_bytes >= g_next_progress) {
        uint8_t pct = (uint8_t)((100ULL * g_received_bytes) / g_total_size);
        if (pct > 100) pct = 100;
        send_progress(g_received_bytes);
        screen_ota_updating::set_progress(pct);  // same task as lv_timer_handler
        // Advance past every threshold this chunk crossed (no spam, no loop hang
        // — g_progress_step >= 1 guarantees termination).
        while (g_received_bytes >= g_next_progress) g_next_progress += g_progress_step;
        LOG("[ota] progress: %u%%\n", (unsigned)pct);
    }
    return true;
}

static void handle_end() {
    if (g_ota_state != OtaState::AwaitingData) return;

    // Short image: host sent END before all declared bytes arrived.
    if (g_received_bytes != g_total_size) {
        LOG("[ota] END with %u/%u bytes — truncated\n",
            (unsigned)g_received_bytes, (unsigned)g_total_size);
        fail_in_ota("truncated");
        return;
    }

    uint8_t computed[32];
    mbedtls_sha256_finish(&g_sha256_ctx, computed);
    mbedtls_sha256_free(&g_sha256_ctx);
    g_sha_active = false;

    if (memcmp(computed, g_expected_sha256, 32) != 0) {
        LOG("[ota] FAILED: hash_mismatch\n");
        fail_in_ota("hash_mismatch");
        return;
    }

    // Structural sanity: must be a real ESP32-S3 app image (header magic 0xE9).
    if (g_stage_buf[0] != 0xE9) {
        LOG("[ota] FAILED: not an app image (magic=0x%02X)\n", g_stage_buf[0]);
        fail_in_ota("bad_image");
        return;
    }

    // Version consistency check: the version stamped INSIDE the binary must
    // match what Orion declared at BEGIN. A mismatch means the wrong/mislabeled
    // image was sent — that's the ONLY version-based rejection. Same-version
    // re-installs and downgrades are allowed as long as the label is honest.
    char img_ver[24] = {};
    if (!extract_image_version(g_stage_buf, g_total_size, img_ver, sizeof(img_ver))) {
        LOG("[ota] FAILED: no version marker in image (can't verify claim '%s')\n",
            g_claimed_version);
        fail_in_ota("version_mismatch");
        return;
    }
    if (strcmp(img_ver, g_claimed_version) != 0) {
        LOG("[ota] FAILED: version_mismatch (binary='%s' BEGIN claim='%s')\n",
            img_ver, g_claimed_version);
        fail_in_ota("version_mismatch");
        return;
    }
    strncpy(g_new_version, img_ver, sizeof(g_new_version) - 1);
    g_new_version[sizeof(g_new_version) - 1] = '\0';
    LOG("[ota] version verified: %s (binary matches BEGIN claim)\n", g_new_version);

    // Image received and verified — go straight to the "Installing firmware"
    // frame (transforming the live download screen) and arm the flash commit on
    // the next poll() tick, after the linger lets that frame + countdown bar
    // render. No user confirmation step.
    screen_ota_updating::set_progress(100);
    screen_ota_updating::set_installing(COMMIT_LINGER_MS);
    g_ota_state      = OtaState::Installing;
    g_commit_pending = true;
    g_commit_at      = millis() + COMMIT_LINGER_MS;
    LOG("[ota] image verified — installing\n");
}

// Flash the staged PSRAM image. Runs once, on the poll() tick AFTER handle_end,
// so the "Installing…" frame has rendered. Halts the LCD (PSRAM-DMA vs flash
// bus contention), bursts the image to flash, then reboots into it.
static void do_commit() {
    g_commit_pending = false;

    // Make the commit a clean single-threaded operation: tear down BLE and halt
    // the LCD so nothing executes non-IRAM code or races a flash/NVS write while
    // Update.write() has the MSPI cache disabled. We reboot right after, so both
    // are one-way.
    lcd_panel::blackout();        // fill framebuffer black — clean visual cut before stop
    ble_manager::quiesce_for_commit();
    lcd_panel::stop();            // halt LCD_CAM hardware (stops PSRAM-DMA/flash bus contention)

    bool ok = Update.begin((size_t)g_total_size);
    if (ok) {
        const uint32_t CHUNK = 32768;
        for (uint32_t off = 0; off < g_total_size && ok; off += CHUNK) {
            uint32_t c = g_total_size - off;
            if (c > CHUNK) c = CHUNK;
            ok = (Update.write(g_stage_buf + off, c) == c);
            vTaskDelay(1);        // yield between bursts so watchdogs stay fed
        }
        if (ok) ok = Update.end(true);
    }
    free_stage();

    if (!ok) {
        // Commit failed with the LCD already halted — can't show the error
        // screen, so reboot (the active slot is untouched; old firmware boots).
        LOG("[ota] FAILED: commit err=%u — rebooting\n", (unsigned)Update.getError());
        send_failed("flash_error");
        Serial.flush();
        delay(200);
        ESP.restart();
        return;
    }

    // Persist the post-update ack so the new firmware shows "Firmware updated"
    // on its first boot (sticky until the user taps Close).
    nvs::set_ota_ack(g_new_version);

    send_empty_response(OTA_OP_VALIDATED);
    LOG("[ota] VALIDATED — restarting\n");
    Serial.flush();
    delay(200);
    ESP.restart();
}

// Byte-at-a-time parser for header + control frames only.
static void parse_byte(uint8_t b) {
    switch (g_parse) {
        case ParseState::WaitMagic0:
            if (b == OTA_MAGIC_0) g_parse = ParseState::WaitMagic1;
            break;

        case ParseState::WaitMagic1:
            g_parse = (b == OTA_MAGIC_1) ? ParseState::WaitOp
                                         : ParseState::WaitMagic0;
            break;

        case ParseState::WaitOp:
            g_current_op   = b;
            g_payload_len  = 0;
            g_payload_read = 0;
            g_parse        = ParseState::WaitLen0;
            break;

        case ParseState::WaitLen0:
            g_payload_len = b;
            g_parse       = ParseState::WaitLen1;
            break;

        case ParseState::WaitLen1:
            g_payload_len |= ((uint32_t)b << 8);
            g_parse        = ParseState::WaitLen2;
            break;

        case ParseState::WaitLen2:
            g_payload_len |= ((uint32_t)b << 16);
            if (g_payload_len == 0) {
                if (g_current_op == OTA_OP_END) handle_end();
                g_parse = ParseState::WaitMagic0;
            } else if (g_current_op == OTA_OP_DATA) {
                // poll() bulk-consumes the payload from here.
                g_parse = ParseState::ReadDataStream;
            } else {
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
                switch (g_current_op) {
                    case OTA_OP_BEGIN: handle_begin(g_ctrl_buf, g_ctrl_pos); break;
                    case OTA_OP_END:   handle_end(); break;
                    default: break;
                }
                g_parse = ParseState::WaitMagic0;
            }
            break;

        case ParseState::ReadDataStream:
            // Unreachable — poll() owns DATA. Present only for switch completeness.
            break;
    }
}

} // namespace

// ── Public API ─────────────────────────────────────────────────────────────

namespace ota_receiver {

void init() {
    g_ota_state = OtaState::Idle;
    g_parse     = ParseState::WaitMagic0;
    // Referencing g_ori_fw_marker here keeps --gc-sections from dropping it
    // (nothing else reads the running build's own copy — it's only ever scanned
    // out of OTHER builds' staged images). Also a handy boot sanity line.
    LOG("[ota] receiver ready (%s)\n", g_ori_fw_marker);
}

void poll() {
    // A commit is armed; once the "Installing…" frame has rendered and lingered,
    // halt the LCD, flash the staged image, and reboot.
    if (g_commit_pending && (int32_t)(millis() - g_commit_at) >= 0) {
        do_commit();
        return;
    }

    const uint32_t start = millis();
    static uint8_t rx[RX_CHUNK];

    while (Serial.available() > 0) {
        // When idle, never steal a byte that isn't the start of an OTA frame —
        // leave it for the debug serial consumer.
        if (g_parse == ParseState::WaitMagic0 &&
            (uint8_t)Serial.peek() != OTA_MAGIC_0) {
            break;
        }

        if (g_parse == ParseState::ReadDataStream) {
            // Bulk path: one Serial.read + one Update.write + one sha256_update.
            uint32_t remaining = g_payload_len - g_payload_read;
            uint32_t avail     = (uint32_t)Serial.available();
            uint32_t want      = remaining;
            if (want > avail)     want = avail;
            if (want > RX_CHUNK)  want = RX_CHUNK;

            int got = Serial.read(rx, want);
            if (got <= 0) break;

            g_last_rx_ms = millis();
            if (!feed_data(rx, (uint32_t)got)) break;  // aborted inside
            g_payload_read += (uint32_t)got;
            if (g_payload_read >= g_payload_len) g_parse = ParseState::WaitMagic0;
        } else {
            int b = Serial.read();
            if (b < 0) break;
            if (g_ota_state == OtaState::AwaitingData) g_last_rx_ms = millis();
            parse_byte((uint8_t)b);
        }

        // Cooperative yield: hand the loop back to LVGL + the idle task.
        if (millis() - start >= POLL_BUDGET_MS) break;
    }

    // Stall watchdog — USB unplugged or host hang mid-download (ota.md:
    // "partial image discarded"). Only fires during the active download, not
    // during the brief Installing linger (state Installing).
    if (g_ota_state == OtaState::AwaitingData &&
        (uint32_t)(millis() - g_last_rx_ms) > OTA_STALL_TIMEOUT_MS) {
        fail_in_ota("usb_timeout");
    }
}

bool is_active() { return g_ota_state != OtaState::Idle; }

bool is_busy() { return g_parse != ParseState::WaitMagic0; }

// "Close" tapped on the Update failed screen → drop everything, resume runtime.
void dismiss_error() {
    if (g_ota_state != OtaState::Failed) return;
    LOG("[ota] error dismissed\n");
    g_ota_state = OtaState::Idle;
    g_parse     = ParseState::WaitMagic0;
    state_machine::on_reconnect_end();   // re-evaluate → runtime screen
}

} // namespace ota_receiver
