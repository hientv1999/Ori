// Ori BLE firmware-update receiver (ota.md).
//
// Wire format, op set, and reason codes: see ota_receiver.h.
//
// Concurrency / timing model (this is the part that must not regress):
//   - on_control_write()/on_data_write() run on the NIMBLE HOST TASK. They may
//     only do pure computation, memcpy into the (already allocated) PSRAM
//     staging buffer, and send notifications. Anything touching LVGL, NVS, or
//     the heap is deferred to the main task — an NVS write from the host task
//     disables the MSPI cache under the LCD's DMA (hardware.md).
//   - Everything deferred lands in poll(), called from loop() on the Arduino
//     main task: PSRAM allocation, the SHA-256 pass, screen transitions, the
//     no-progress watchdog, and the flash commit.
//   - The data path is deliberately allocation-free and lock-free. `g_received`
//     is the single hand-off word: written only by the host task while
//     streaming, read by the main task to drive the ring and the watchdog.
//     A torn read is impossible (aligned 32-bit) and a stale read costs at most
//     one 10 ms tick of ring lag.
//
// PSRAM-staging model (keeps the progress ring live):
//
// The RGB panel has no frame memory and scans its framebuffer out of PSRAM
// continuously; PSRAM reads and flash writes contend on the shared MSPI bus, so
// the display and flash writes can't run at the same instant. Instead of
// writing flash during the download (which would force the screen dark the
// whole time), we stream the whole image into a PSRAM buffer first — no flash
// writes, so the LCD keeps refreshing and the progress ring advances live. Only
// at END, once the image is received and its hash checked, do we halt the LCD
// and burst the staged image to flash (a few seconds dark), then reboot.
//
// Failures during the download phase (truncated / hash / overflow / stall /
// link loss) free the buffer and resume runtime with the display still alive —
// no reboot. Only the successful flash commit halts the LCD and reboots.

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

// Defined in ble_manager.cpp. The control-write callback runs on the NimBLE
// host task; these post onto the same main-task event queue every other GATT
// handler uses, so run_begin()/run_end()/run_abort() execute where PSRAM
// allocation, LVGL, and NVS are safe.
void ble_post_fw_update_begin_event();
void ble_post_fw_update_end_event();
void ble_post_fw_update_abort_event();

// ── Tunables ───────────────────────────────────────────────────────────────

// PROGRESS cadence. Doubles as the sender's flow-control ack, so it must fire
// often enough in BYTES that Orion can keep a tight write window without ever
// running more than that window ahead (ota.md's sender guide). One notify per
// integer percent of a ~1.5 MB image is ~15 KB, so the byte cap below is what
// actually governs on anything smaller.
static const uint32_t PROGRESS_INTERVAL_PCT = 1;
static const uint32_t PROGRESS_MAX_BYTES    = 16384;

// No-progress watchdog. Over BLE a healthy transfer delivers fragments every
// connection interval (7.5-15 ms while boosted), but the link can legitimately
// stall for a second or two — Windows rescheduling, the phone link stealing
// radio time, a brief range dip. 10 s matches the chunked-transfer timeout the
// rest of the protocol already uses (ble-protocol.md §5) and is ~100x the
// largest normal gap.
static const uint32_t OTA_STALL_TIMEOUT_MS = 10000;

// Minimum gap between RESUME notifies. When the sender runs past a dropped
// fragment, every frame still in flight arrives with the wrong offset; one
// RESUME per burst is enough for it to rewind, and repeating every 200 ms
// self-heals if that notify is itself lost.
static const uint32_t RESUME_MIN_INTERVAL_MS = 200;

// How long the "Installing… screen goes dark" frame stays up before the flash
// commit blanks the panel — long enough for the user to read it (and the
// countdown bar to run), and comfortably long enough for the VALIDATED notify
// sent just before it to reach Orion before the stack is torn down.
static const uint32_t COMMIT_LINGER_MS = 3500;

// Embedded firmware-version marker. Compiled into every Ori image so the OTA
// receiver can read the *incoming* image's own version straight from the staged
// binary (authoritative) instead of trusting the version string Orion sends in
// BEGIN. The stock esp_app_desc.version on this precompiled Arduino core is its
// git hash ("arduino-lib-builder"), not ours — so we stamp our own.
//
// It MUST be a single contiguous char[] (one string literal): a multi-field
// struct gets split/reordered by the optimizer (-O2 ICF), scattering the
// members. The scanner (extract_image_version) finds the "OriFwVer=" prefix and
// reads the null-terminated version after it.
extern "C" __attribute__((used))
const char g_ori_fw_marker[] = "OriFwVer=" ORI_FW_VERSION;

// ── State machine ──────────────────────────────────────────────────────────

// OTA lifecycle. is_active() (sticky OTA screen + BLE NACK gate) is true for
// any state other than Idle.
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

volatile OtaState g_ota_state = OtaState::Idle;

uint8_t* g_stage_buf     = nullptr;  // PSRAM staging buffer (g_total_size bytes)
uint32_t g_total_size    = 0;
volatile uint32_t g_received     = 0;  // host task writes, main task reads
volatile uint32_t g_last_rx_ms   = 0;  // millis() of the last accepted frame
uint32_t g_next_progress = 0;        // byte threshold for the next PROGRESS
uint32_t g_progress_step = 1;        // bytes per PROGRESS_INTERVAL_PCT (>=1)
uint32_t g_last_resume_ms = 0;
uint8_t  g_last_ring_pct = 0xFF;     // last value pushed to the progress ring

uint8_t g_expected_sha256[32] = {};
char    g_claimed_version[24] = {};  // version Orion declared at BEGIN (the claim)
char    g_new_version[24]     = {};  // version read from the binary — shown on the ack

// BEGIN fields parsed on the NimBLE task, consumed by run_begin() on the main
// task. Only one BEGIN can ever be in flight (a second is rejected as "busy"
// before it reaches here), so a single slot is enough.
struct {
    char     version[24];
    uint32_t total_size;
    uint8_t  sha256[32];
} g_pending_begin = {};

// Deferred failure. Set from either task with a string literal (always valid);
// drained by poll() on the main task, which owns the notify + error screen.
const char* volatile g_pending_fail = nullptr;

bool     g_commit_pending = false;   // END ok → flash once g_commit_at passes
uint32_t g_commit_at      = 0;

static void free_stage() {
    if (g_stage_buf) {
        heap_caps_free(g_stage_buf);
        g_stage_buf = nullptr;
    }
}

// Request the failure path. Safe from either task — poll() does the work.
// First caller wins: a link drop racing a stall watchdog shouldn't overwrite
// the more specific reason with the more generic one.
static void request_fail(const char* code) {
    if (!g_pending_fail) g_pending_fail = code;
}

// Scan a staged image for the embedded version marker ("OriFwVer=<version>")
// and copy the version into `out` (null-terminated). Returns false if not found
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
// screen. (The wire still carries the terse code in the REJECT/FAILED notify.)
static const char* friendly_reason(const char* code) {
    static const struct { const char* code; const char* message; } kReasons[] = {
        { "ble_timeout",      "Lost contact with Orion — move Ori closer to your PC and try again" },
        { "link_lost",        "Orion disconnected during the update. Try again" },
        { "aborted",          "The update was cancelled" },
        { "truncated",        "The download didn't finish — try again from Orion" },
        { "hash_mismatch",    "The update was corrupted in transfer — try again" },
        { "size_overflow",    "The update didn't transfer correctly — try again" },
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
static void btn_dismiss_cb(lv_event_t*) { ota_receiver::dismiss_error(); }

// Show the "Update failed" screen and park in Failed until the user taps Close.
// Main task only. The terminal REJECT/FAILED notify must already have been sent.
static void show_error_screen(const char* code) {
    free_stage();
    gatt_server::set_ota_active(false);
    // Lift OTA quiet mode: restore the normal connection interval, re-arm
    // advertising, and resume ANCS processing (which brings the iPhone back).
    // Safe no-op for pre-accept rejects (too_large / no_memory), where quiet
    // mode was never entered.
    ble_manager::set_ota_link_fast(false);
    ble_manager::set_ota_transfer_quiet(false);
    g_ota_state = OtaState::Failed;
    state_machine::ota_show(
        screen_ota_updating::create_error(friendly_reason(code), btn_dismiss_cb));
    LOG("[ota] error: %s\n", code);
}

// Flash the staged PSRAM image. Runs once, on the poll() tick after the
// Installing frame has rendered and lingered. Halts the LCD (PSRAM-DMA vs flash
// bus contention), bursts the image to flash, then reboots into it.
//
// VALIDATED was already notified at the end of run_end(): the BLE stack is torn
// down below and cannot report anything from here on. A flash failure therefore
// surfaces to Orion as "rebooted, still running the old version", which is
// exactly what it checks after every update (ota.md).
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
        // Commit failed with the LCD already halted and BLE already down — no
        // way to show or send anything. Reboot; the active slot is untouched, so
        // the old firmware comes back up and Orion sees an unchanged version.
        LOG("[ota] FAILED: commit err=%u — rebooting\n", (unsigned)Update.getError());
        delay(200);
        ESP.restart();
        return;
    }

    // Persist the post-update ack so the new firmware shows "Firmware updated"
    // on its first boot (sticky until the user taps Close).
    nvs::set_ota_ack(g_new_version);

    LOG("[ota] committed — restarting into %s\n", g_new_version);
    delay(200);
    ESP.restart();
}

} // namespace

// ── Public API ─────────────────────────────────────────────────────────────

namespace ota_receiver {

void init() {
    g_ota_state    = OtaState::Idle;
    g_pending_fail = nullptr;
    // Referencing g_ori_fw_marker here keeps --gc-sections from dropping it
    // (nothing else reads the running build's own copy — it's only ever scanned
    // out of OTHER builds' staged images). Also a handy boot sanity line.
    LOG("[ota] BLE receiver ready (%s)\n", g_ori_fw_marker);
}

// ── Firmware Update Control (char 0014) — NimBLE host task ─────────────────

void on_control_write(const uint8_t* payload, uint16_t len) {
    CborParser parser;
    CborValue  root, map_val;
    if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) {
        gatt_server::notify_fw_reason("REJECT", "cbor_decode");
        return;
    }
    if (!cbor_value_is_map(&root)) {
        gatt_server::notify_fw_reason("REJECT", "not_map");
        return;
    }

    char     op[16]       = {};
    char     version[24]  = {};
    uint64_t total_size   = 0;
    uint8_t  sha256[32]   = {};
    bool     got_version  = false;
    bool     got_size     = false;
    bool     got_sha      = false;

    cbor_value_enter_container(&root, &map_val);
    while (!cbor_value_at_end(&map_val)) {
        char   key[8]  = {};
        size_t key_len = sizeof(key) - 1;
        if (cbor_value_is_text_string(&map_val)) {
            cbor_value_copy_text_string(&map_val, key, &key_len, &map_val);
        } else { cbor_value_advance(&map_val); continue; }
        if (cbor_value_at_end(&map_val)) break;

        if (strcmp(key, "o") == 0 && cbor_value_is_text_string(&map_val)) {
            size_t sz = sizeof(op) - 1;
            cbor_value_copy_text_string(&map_val, op, &sz, nullptr);
        } else if (strcmp(key, "v") == 0 && cbor_value_is_text_string(&map_val)) {
            size_t sz = sizeof(version) - 1;
            cbor_value_copy_text_string(&map_val, version, &sz, nullptr);
            got_version = true;
        } else if (strcmp(key, "n") == 0 && cbor_value_is_unsigned_integer(&map_val)) {
            cbor_value_get_uint64(&map_val, &total_size);
            got_size = true;
        } else if (strcmp(key, "h") == 0 && cbor_value_is_byte_string(&map_val)) {
            size_t sha_len = sizeof(sha256);
            cbor_value_copy_byte_string(&map_val, sha256, &sha_len, nullptr);
            got_sha = (sha_len == 32);
        }
        if (!cbor_value_at_end(&map_val)) cbor_value_advance(&map_val);
    }

    if (strcmp(op, "BEGIN") == 0) {
        // Reject a second BEGIN while a transfer is live — never restart
        // mid-flow. (No error screen: don't disrupt the active update.)
        // Failed is deliberately NOT live: a retry from Orion supersedes an
        // error screen nobody has dismissed yet. Requiring someone to walk over
        // and tap Close made sense when they had to walk over to plug a cable
        // in anyway; it is the wrong gate on a wireless retry.
        if (g_ota_state != OtaState::Idle && g_ota_state != OtaState::Failed) {
            gatt_server::notify_fw_reason("REJECT", "busy");
            return;
        }
        // `v` is required: the BEGIN claim is one half of the version
        // consistency check (the binary's own embedded version is the other —
        // see run_end()). Without it there's nothing to verify the image against.
        if (!got_version || !got_size || !got_sha) {
            gatt_server::notify_fw_reason("REJECT", "missing_fields");
            return;
        }
        // Clamp here so the main task never sees a size it can't represent;
        // run_begin() re-checks against the real slot capacity.
        if (total_size > 0xFFFFFFFFull) total_size = 0xFFFFFFFFull;
        strncpy(g_pending_begin.version, version, sizeof(g_pending_begin.version) - 1);
        g_pending_begin.version[sizeof(g_pending_begin.version) - 1] = '\0';
        g_pending_begin.total_size = (uint32_t)total_size;
        memcpy(g_pending_begin.sha256, sha256, 32);
        ble_post_fw_update_begin_event();
    } else if (strcmp(op, "END") == 0) {
        ble_post_fw_update_end_event();
    } else if (strcmp(op, "ABORT") == 0) {
        ble_post_fw_update_abort_event();
    } else {
        gatt_server::notify_fw_reason("REJECT", "bad_op");
    }
}

// ── Firmware Update Data (char 0015) — NimBLE host task, hot path ──────────

void on_data_write(const uint8_t* frame, uint16_t len) {
    if (g_ota_state != OtaState::AwaitingData || !g_stage_buf) return;
    if (len <= 4) return;   // header-only frame carries nothing

    const uint32_t offset = (uint32_t)frame[0]
                          | ((uint32_t)frame[1] << 8)
                          | ((uint32_t)frame[2] << 16)
                          | ((uint32_t)frame[3] << 24);
    const uint32_t n        = (uint32_t)len - 4;
    const uint32_t received = g_received;

    // Only the exact next byte position is accepted. A gap (dropped fragment)
    // or a rewind (sender retrying) both land here; answer with the offset we
    // actually want and drop the frame. Rate-limited so the sender's in-flight
    // window doesn't produce one notify per stale frame.
    if (offset != received) {
        uint32_t now = (uint32_t)millis();
        if (now - g_last_resume_ms >= RESUME_MIN_INTERVAL_MS) {
            g_last_resume_ms = now;
            LOG("[ota] resync: want %u got %u\n", (unsigned)received, (unsigned)offset);
            gatt_server::notify_fw_bytes("RESUME", received);
        }
        return;
    }

    // Sender overran the size it declared at BEGIN — protocol error, not a
    // recoverable transport hiccup.
    if (n > g_total_size - received) {
        request_fail("size_overflow");
        return;
    }

    memcpy(g_stage_buf + received, frame + 4, n);
    g_received   = received + n;
    g_last_rx_ms = (uint32_t)millis();

    if (g_received >= g_next_progress) {
        gatt_server::notify_fw_bytes("PROGRESS", g_received);
        // Advance past every threshold this frame crossed (no spam, no loop
        // hang — g_progress_step >= 1 guarantees termination).
        while (g_received >= g_next_progress) g_next_progress += g_progress_step;
    }
}

// ── Deferred control ops — main task ───────────────────────────────────────

void run_begin() {
    // A retry lands here with the previous attempt's error screen still up
    // (see on_control_write) — drop straight back to Idle and rebuild.
    if (g_ota_state == OtaState::Failed) g_ota_state = OtaState::Idle;
    if (g_ota_state != OtaState::Idle) return;   // raced a failure; stay put

    // These rejects ARE user-relevant — surface them on the Update-failed screen
    // (the wire still gets the REJECT notify so Orion knows too).
    const uint32_t OTA_SLOT_SIZE = 3 * 1024 * 1024;
    if (g_pending_begin.total_size == 0 || g_pending_begin.total_size > OTA_SLOT_SIZE) {
        gatt_server::notify_fw_reason("REJECT", "too_large");
        show_error_screen("too_large");
        return;
    }

    // Stage the whole image in PSRAM. No flash is touched during the download,
    // so the LCD keeps running and the progress ring stays live. Flash is
    // written in one burst at END (see do_commit).
    free_stage();
    g_stage_buf = (uint8_t*)heap_caps_malloc((size_t)g_pending_begin.total_size,
                                             MALLOC_CAP_SPIRAM);
    if (!g_stage_buf) {
        LOG("[ota] PSRAM staging alloc failed (%u bytes)\n",
            (unsigned)g_pending_begin.total_size);
        gatt_server::notify_fw_reason("REJECT", "no_memory");
        show_error_screen("no_memory");
        return;
    }

    g_total_size    = g_pending_begin.total_size;
    g_received      = 0;
    g_progress_step = g_total_size / (100 / PROGRESS_INTERVAL_PCT);
    if (g_progress_step == 0) g_progress_step = 1;             // tiny image guard
    if (g_progress_step > PROGRESS_MAX_BYTES) g_progress_step = PROGRESS_MAX_BYTES;
    g_next_progress  = g_progress_step;
    g_last_rx_ms     = (uint32_t)millis();
    g_last_resume_ms = 0;
    g_last_ring_pct  = 0xFF;
    g_pending_fail   = nullptr;
    memcpy(g_expected_sha256, g_pending_begin.sha256, 32);
    strncpy(g_claimed_version, g_pending_begin.version, sizeof(g_claimed_version) - 1);
    g_claimed_version[sizeof(g_claimed_version) - 1] = '\0';

    // No version decision here — we can't read the image's embedded version
    // until it's downloaded. run_end() compares this BEGIN claim against the
    // version stamped inside the binary and rejects only if they DISAGREE.

    // Flip the state as soon as the buffer and counters are ready: it is what
    // opens the host task's data path onto g_stage_buf, and what makes
    // is_active() true for everything below (state_machine's priority
    // evaluation included). Safe to do before the screen exists — the data
    // path only memcpys and notifies; the ring is driven from poll(), on this
    // same task, which cannot run until run_begin() returns.
    g_ota_state = OtaState::AwaitingData;

    gatt_server::set_ota_active(true);   // NACK every other BLE data write
    // Stop advertising, drop the phone link, and suspend ANCS for the whole
    // download — a reconnect ceremony or a notification storm competing for the
    // same radio is what turns a 40-second transfer into a stalled one
    // (ble_manager.h's set_ota_transfer_quiet doc comment). Reversed by
    // show_error_screen() on any failure; a successful commit reboots instead.
    ble_manager::set_ota_transfer_quiet(true);
    // Ask Orion for the fastest connection interval it will grant — this is the
    // single biggest lever on transfer time.
    ble_manager::set_ota_link_fast(true);
    state_machine::on_ota_begin();       // full-screen OTA takeover (Updating)
    screen_ota_updating::set_progress(0);

    gatt_server::notify_fw_status("READY");
    LOG("[ota] BEGIN accepted: size=%u ver=%s\n",
        (unsigned)g_total_size, g_claimed_version);
}

void run_end() {
    if (g_ota_state != OtaState::AwaitingData) return;

    // Short image: sender sent END before all declared bytes arrived.
    if (g_received != g_total_size) {
        LOG("[ota] END with %u/%u bytes — truncated\n",
            (unsigned)g_received, (unsigned)g_total_size);
        request_fail("truncated");
        return;
    }

    // Hash the staged image in one pass. Streaming the hash during the download
    // would have to be rewound on every RESUME; over a 1.5 MB buffer this pass
    // costs a few tens of ms, once, on the frame that's about to be replaced by
    // the Installing screen anyway.
    uint8_t computed[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, g_stage_buf, g_total_size);
    mbedtls_sha256_finish(&ctx, computed);
    mbedtls_sha256_free(&ctx);

    if (memcmp(computed, g_expected_sha256, 32) != 0) {
        LOG("[ota] FAILED: hash_mismatch\n");
        request_fail("hash_mismatch");
        return;
    }

    // Structural sanity: must be a real ESP32-S3 app image (header magic 0xE9).
    if (g_stage_buf[0] != 0xE9) {
        LOG("[ota] FAILED: not an app image (magic=0x%02X)\n", g_stage_buf[0]);
        request_fail("bad_image");
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
        request_fail("version_mismatch");
        return;
    }
    if (strcmp(img_ver, g_claimed_version) != 0) {
        LOG("[ota] FAILED: version_mismatch (binary='%s' BEGIN claim='%s')\n",
            img_ver, g_claimed_version);
        request_fail("version_mismatch");
        return;
    }
    strncpy(g_new_version, img_ver, sizeof(g_new_version) - 1);
    g_new_version[sizeof(g_new_version) - 1] = '\0';
    LOG("[ota] version verified: %s (binary matches BEGIN claim)\n", g_new_version);

    // Tell Orion now, while the link is still up: the commit below tears the BLE
    // stack down and reboots, so this is the last frame Ori can send. VALIDATED
    // means "image accepted, installing" — Orion confirms the running version
    // over BLE after the reboot (ota.md), which is what actually proves success.
    gatt_server::notify_fw_validated(g_new_version);

    // Image received and verified — go straight to the "Installing firmware"
    // frame (transforming the live download screen) and arm the flash commit on
    // a later poll() tick, after the linger lets that frame + countdown bar
    // render. No user confirmation step.
    screen_ota_updating::set_progress(100);
    screen_ota_updating::set_installing(COMMIT_LINGER_MS);
    g_ota_state      = OtaState::Installing;
    g_commit_pending = true;
    g_commit_at      = millis() + COMMIT_LINGER_MS;
    LOG("[ota] image verified — installing\n");
}

void run_abort() {
    // Dismissing the Update-failed screen in Orion clears it here too, so the
    // user doesn't acknowledge one failure twice, once per device. ABORT
    // already means "the sender is done with this update"; with nothing in
    // flight the only thing left to abandon is the error screen. Same spirit
    // as a retry superseding an undismissed error (ota.md).
    if (g_ota_state == OtaState::Failed) {
        dismiss_error();
        return;
    }
    if (g_ota_state != OtaState::AwaitingData) return;  // too late once installing
    request_fail("aborted");
}

void on_orion_disconnected() {
    // Only the download needs the link. Past verification the image is already
    // staged and hashed, so let the commit run to completion — Orion will find
    // the new version when it reconnects.
    if (g_ota_state == OtaState::AwaitingData) request_fail("link_lost");
}

// ── Main-task tick ─────────────────────────────────────────────────────────

void poll() {
    // A failure was raised (by either task) — send the terminal notify and put
    // the error screen up. Done here so the LVGL/NVS work never runs on the
    // NimBLE host task.
    if (g_pending_fail) {
        const char* code = g_pending_fail;
        g_pending_fail = nullptr;
        if (g_ota_state == OtaState::AwaitingData) {
            gatt_server::notify_fw_reason("FAILED", code);
            show_error_screen(code);
        }
        return;
    }

    // A commit is armed; once the "Installing…" frame has rendered and lingered,
    // halt the LCD, flash the staged image, and reboot.
    if (g_commit_pending && (int32_t)(millis() - g_commit_at) >= 0) {
        do_commit();
        return;
    }

    if (g_ota_state != OtaState::AwaitingData) return;

    // Once per transfer, after the stream is actually moving, report what the
    // link really negotiated. set_ota_link_fast() only asks; the central can
    // silently decline, and throughput is bounded by (bytes per connection
    // event) x (events per second) — so this is the first thing to read when a
    // transfer is slower than expected.
    // Drive the progress ring from the host task's byte counter, one update per
    // integer percent (the ring itself only has 100 states).
    if (g_total_size) {
        uint8_t pct = (uint8_t)((100ULL * g_received) / g_total_size);
        if (pct > 99) pct = 99;   // 100% is reserved for the verified image
        if (pct != g_last_ring_pct) {
            g_last_ring_pct = pct;
            screen_ota_updating::set_progress(pct);
        }
    }

    // No-progress watchdog — Orion crashed, went out of range, or stopped
    // sending mid-download (ota.md: "partial image discarded"). Only runs during
    // the active download, never during the Installing linger.
    if ((uint32_t)(millis() - g_last_rx_ms) > OTA_STALL_TIMEOUT_MS) {
        request_fail("ble_timeout");
    }
}

bool is_active() { return g_ota_state != OtaState::Idle; }

// "Close" tapped on the Update failed screen → drop everything, resume runtime.
void dismiss_error() {
    if (g_ota_state != OtaState::Failed) return;
    LOG("[ota] error dismissed\n");
    g_ota_state = OtaState::Idle;
    // Mirror it to Orion so its own Update-failed panel closes too — the user
    // acknowledges the failure once, on whichever device is in front of them.
    // Sent unconditionally: when Orion initiated this (via ABORT) it has
    // already closed and simply ignores the echo.
    gatt_server::notify_fw_status("DISMISSED");
    state_machine::on_reconnect_end();   // re-evaluate → runtime screen
}

} // namespace ota_receiver
