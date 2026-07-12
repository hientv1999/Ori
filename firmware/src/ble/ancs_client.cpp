// Ori ANCS client — Apple Notification Center Service subscriber.
//
// ANCS service UUID:  7905F431-B5CE-4E99-A40F-4B1E122D00D0
// Notification Source (NS): 9FBF120D-6301-42D9-8C58-25E699A21DBD
// Control Point (CP):       69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
// Data Source (DS):         22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
//
// Notification-source event byte layout (8 bytes):
//   0: EventID      (0=Added, 1=Modified, 2=Removed)
//   1: EventFlags
//   2: CategoryID
//   3: CategoryCount
//   4-7: NotificationUID (uint32 LE)
//
// GetNotificationAttributes command (sent to CP):
//   0: CommandID = 0x00
//   1-4: NotificationUID (uint32 LE)
//   5+: AttributeID list (0x01=Title, 0x03=Message, with length for 01 and 03)
//
// Icons only — notification content displayed only on user tap via modal.

#include "ble/ancs_client.h"
#include "ble/ble_manager.h"
#include "ble/gatt_server.h"

#include <Arduino.h>
#include "ori_log.h"
#include <NimBLEDevice.h>
#include <NimBLEClient.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>   // settimeofday (iPhone CTS backup clock)

#include "app_state.h"
#include "state_machine.h"
#include "ota_receiver.h"
#include "assets/ancs_icons.h"
#include "screens/modal_ancs_list.h"
#include "screens/modal_ancs_notification.h"
#include "screens/modal_incoming_call.h"
#include "screens/modal_iphone_info.h"
#include "ui_helpers.h"
#include "widgets/widget_status_bar.h"

// ── ANCS UUIDs ────────────────────────────────────────────────────────────

#define ANCS_SVC_UUID "7905F431-B5CE-4E99-A40F-4B1E122D00D0"
#define ANCS_NS_UUID  "9FBF120D-6301-42D9-8C58-25E699A21DBD"
#define ANCS_CP_UUID  "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9"
#define ANCS_DS_UUID  "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB"

// ── Bundle ID → icon token mapping ───────────────────────────────────────

struct BundleMap {
    const char* bundle_id;
    const char* token;
    const char* name;     // human-readable app name for the detail modal
};

static const BundleMap k_bundle_map[] = {
    { "com.google.Gmail",               "gmail",       "Gmail"       },
    { "com.facebook.Messenger",         "messenger",   "Messenger"   },
    { "com.burbn.instagram",            "instagram",   "Instagram"   },
    { "com.facebook.Facebook",          "facebook",    "Facebook"    },
    { "net.whatsapp.WhatsApp",          "whatsapp",    "WhatsApp"    },
    { "com.tinyspeck.chatlyio",         "slack",       "Slack"       },
    { "com.atebits.Tweetie2",           "twitter",     "X"           },
    { "com.microsoft.skype.teams",      "teams",       "Teams"       },
    { "com.apple.MobileSMS",            "sms",         "Messages"    },
    { "com.apple.mobilephone",          "phone",       "Phone"       },
    { "com.hammerandchisel.discord",    "discord",     "Discord"     },
    { "ph.telegra.Telegraph",           "telegram",    "Telegram"    },
    { "com.google.ios.youtube",         "youtube",       "YouTube"       },
    { "com.google.ios.youtubemusic",    "youtube_music", "YouTube Music" },
    { "com.zhiliaoapp.musically",       "tiktok",      "TikTok"      },
    { "com.ss.iphone.ugc.Ame",          "tiktok",      "TikTok"      },
    { "com.spotify.client",             "spotify",     "Spotify"     },
    { "com.tencent.xin",                "wechat",      "WeChat"      },
    { "jp.naver.line",                  "line",        "LINE"        },
    { "us.zoom.videomeetings",          "zoom",        "Zoom"        },
    { "com.microsoft.office.outlook",   "outlook",     "Outlook"     },
    { "com.toyopagroup.picaboo",        "snapchat",    "Snapchat"    },
    { "com.google.hangouts.meet",       "google_meet", "Google Meet" },
    { "com.apple.facetime",             "facetime",    "FaceTime"    },
    { "com.linkedin.LinkedIn",          "linkedin",    "LinkedIn"    },
    { "com.reddit.Reddit",              "reddit",      "Reddit"      },
    { "com.burbn.barcelona",            "threads",     "Threads"     },
    { "tv.twitch.mobile.watchlive",     "twitch",      "Twitch"      },
    { "com.ubercab.UberClient",         "uber",        "Uber"        },
    { "com.apple.Music",                "apple_music", "Apple Music" },
    { "com.amazon.Amazon",              "amazon",      "Amazon"      },
    { "com.viber",                      "viber",       "Viber"       },
    { "com.anthropic.claude",           "claude",      "Claude"      },
    { "com.openai.chat",                "chatgpt",     "ChatGPT"     },
    { "com.google.Maps",                "google_map",    "Google Maps"   },
    { "com.google.photos",              "google_photos", "Google Photos" },
    { "com.apple.Health",               "health",        "Health"        },
    { "com.apple.mobilecal",            "apple_calendar",          "Calendar"               },
    { "com.apple.findmy",               "apple_findmy",            "Find My"                },
    { "com.apple.mobilemail",           "apple_mail",              "Mail"                   },
    { "com.apple.Maps",                 "apple_maps",              "Maps"                   },
    { "com.apple.reminders",            "apple_reminders",         "Reminders"              },
    { "com.apple.Passbook",             "apple_wallet",            "Wallet"                 },
    { "com.github.stormbreaker.prod",   "github",                  "GitHub"                 },
    { "com.google.authenticator",       "google_authenticator",    "Google Authenticator"   },
    { "com.azure.authenticator",        "microsoft_authenticator", "Microsoft Authenticator"},
    { "notion.id",                      "notion",                  "Notion"                 },
    { "com.venmo.Venmo",                "venmo",                   "Venmo"                  },
    { "com.skype.skype",                "skype",                   "Skype"                  },
    { "com.paypal.PPClient",            "paypal",                  "PayPal"                 },
};
static const size_t k_bundle_map_count =
    sizeof(k_bundle_map) / sizeof(k_bundle_map[0]);

static const BundleMap* lookup_bundle(const char* bundle_id) {
    if (!bundle_id || !bundle_id[0]) return nullptr;
    for (size_t i = 0; i < k_bundle_map_count; ++i) {
        if (strcmp(k_bundle_map[i].bundle_id, bundle_id) == 0) return &k_bundle_map[i];
    }
    return nullptr;
}

static const char* resolve_token(const char* bundle_id) {
    const BundleMap* b = lookup_bundle(bundle_id);
    return b ? b->token : nullptr;
}

// Parse an ANCS Date attribute ("yyyyMMdd'T'HHmmSS", e.g. "20140110T114000").
// Returns the Unix epoch (interpreting the wall-clock time as device-local),
// or 0 if the string is missing/malformed. Also fills hhmm_out with the
// TZ-free "HH:MM" lifted straight from the string (always correct regardless
// of the device clock), used as the display fallback when the clock isn't synced.
static time_t parse_ancs_date(const char* s, char* hhmm_out, size_t hhmm_sz) {
    if (hhmm_out && hhmm_sz) hhmm_out[0] = '\0';
    if (!s || strlen(s) < 15) return 0;
    for (int i = 0; i < 15; ++i) {
        if (i == 8) { if (s[i] != 'T') return 0; }
        else if (s[i] < '0' || s[i] > '9') return 0;
    }
    auto d2 = [](const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); };
    struct tm tmv = {};
    tmv.tm_year  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0') - 1900;
    tmv.tm_mon   = d2(s + 4) - 1;
    tmv.tm_mday  = d2(s + 6);
    tmv.tm_hour  = d2(s + 9);
    tmv.tm_min   = d2(s + 11);
    tmv.tm_sec   = d2(s + 13);
    tmv.tm_isdst = -1;
    if (hhmm_out && hhmm_sz >= 6)
        snprintf(hhmm_out, hhmm_sz, "%c%c:%c%c", s[9], s[10], s[11], s[12]);
    time_t e = mktime(&tmv);
    return (e < 0) ? 0 : e;
}

namespace {

// ── Live queue ────────────────────────────────────────────────────────────

ancs_client::QueueEntry g_queue[app_state::MAX_ANCS_NOTIFICATIONS];
size_t                  g_queue_count = 0;

// ── BLE handles ──────────────────────────────────────────────────────────

uint16_t g_conn_handle = 0xFFFF;
NimBLEClient*           g_client      = nullptr;
NimBLERemoteService*    g_ancs_svc    = nullptr;
NimBLERemoteCharacteristic* g_ns_char = nullptr;
NimBLERemoteCharacteristic* g_cp_char = nullptr;
NimBLERemoteCharacteristic* g_ds_char = nullptr;

// ── DS reassembly ─────────────────────────────────────────────────────────

// The Data Source delivers attribute data across multiple notifications.
// We accumulate until we see all requested attributes.
uint8_t  g_ds_buf[1024];
size_t   g_ds_len = 0;
uint32_t g_pending_uid = 0;

// ── NS/DS deferral queues (host task → main task) ──────────────────────────
//
// NimBLE invokes the NS/DS notify callbacks on the HOST task. Processing them
// there is unsafe: on_notification_source() does a blocking CP write-with-
// response (request_attributes) — which deadlocks, since the host task that
// must process the write response is the one stuck in the callback — and the
// queue_add/remove path calls into LVGL (refresh_active), which must only run
// on the main task. So the callbacks just copy the raw bytes into these
// single-producer/single-consumer rings; ancs_client::poll() drains them on
// the main task, where blocking GATT ops and LVGL are both safe.
//
// NS (tiny, fixed 8-byte events) and DS (larger, multi-fragment attribute
// responses) get SEPARATE rings sized for their own traffic pattern, so a
// burst of one kind can't starve out drops of the other — e.g. a string of
// long-bodied DS responses filling every slot would otherwise crowd out a
// concurrent NS "Added" event, permanently losing that notification (nothing
// ever re-sends a dropped NS event). A shared monotonic sequence number lets
// poll() merge-drain both rings back into true arrival order, matching what
// the single combined ring used to guarantee.
//
// Backlog replay on reconnect can burst far more than a handful of frames
// (10+ notifications × 1 NS event + 2-5 DS fragments each), so both rings are
// sized well above steady-state single-notification traffic.
struct NsRaw {
    uint16_t len;
    uint8_t  buf[8];    // NS event is always exactly 8 bytes (ANCS spec)
    uint32_t seq;
};
struct DsRaw {
    uint16_t len;
    uint8_t  buf[256];  // ≥ one ATT_MTU (247) fragment
    uint32_t seq;
};
constexpr uint8_t NS_Q_SIZE = 48;
constexpr uint8_t DS_Q_SIZE = 64;
NsRaw            g_nsq[NS_Q_SIZE];
volatile uint8_t g_nsq_head = 0;   // consumer (main task)
volatile uint8_t g_nsq_tail = 0;   // producer (host task)
DsRaw            g_dsq[DS_Q_SIZE];
volatile uint8_t g_dsq_head = 0;   // consumer (main task)
volatile uint8_t g_dsq_tail = 0;   // producer (host task)
uint32_t         g_aq_seq = 0;     // shared arrival counter — NS/DS callbacks
                                    // both run on the host task, so this stays
                                    // single-producer even though it's shared

void ns_push(const uint8_t* data, size_t len) {
    uint8_t next = (uint8_t)((g_nsq_tail + 1) % NS_Q_SIZE);
    if (next == g_nsq_head) return;  // full → drop
    NsRaw& e = g_nsq[g_nsq_tail];
    e.len = (len > sizeof(e.buf)) ? (uint16_t)sizeof(e.buf) : (uint16_t)len;
    memcpy(e.buf, data, e.len);
    e.seq = g_aq_seq++;
    g_nsq_tail = next;               // publish only after the slot is filled
}

bool ns_pop(NsRaw& out) {
    if (g_nsq_head == g_nsq_tail) return false;
    out = g_nsq[g_nsq_head];
    g_nsq_head = (uint8_t)((g_nsq_head + 1) % NS_Q_SIZE);
    return true;
}

void ds_push(const uint8_t* data, size_t len) {
    uint8_t next = (uint8_t)((g_dsq_tail + 1) % DS_Q_SIZE);
    if (next == g_dsq_head) return;  // full → drop
    DsRaw& e = g_dsq[g_dsq_tail];
    e.len = (len > sizeof(e.buf)) ? (uint16_t)sizeof(e.buf) : (uint16_t)len;
    memcpy(e.buf, data, e.len);
    e.seq = g_aq_seq++;
    g_dsq_tail = next;               // publish only after the slot is filled
}

bool ds_pop(DsRaw& out) {
    if (g_dsq_head == g_dsq_tail) return false;
    out = g_dsq[g_dsq_head];
    g_dsq_head = (uint8_t)((g_dsq_head + 1) % DS_Q_SIZE);
    return true;
}

// ── Connected phone name (GAP Device Name) ───────────────────────────────

char g_phone_name[64] = {};

// Read the iPhone's GAP Device Name (service 0x1800, char 0x2A00) over the
// encrypted link. iOS returns the personalised name ("Xander's iPhone") to
// bonded peers; unbonded readers only get a generic "iPhone". Synchronous
// GATT read — runs in the same loopTask context as the ANCS discovery.
static void read_phone_name() {
    g_phone_name[0] = '\0';
    if (!g_client) return;

    NimBLERemoteService* gap = g_client->getService(NimBLEUUID((uint16_t)0x1800));
    if (!gap) {
        LOG("[ancs] GAP service not found — phone name unavailable\n");
        return;
    }
    NimBLERemoteCharacteristic* name_chr =
        gap->getCharacteristic(NimBLEUUID((uint16_t)0x2A00));
    if (!name_chr || !name_chr->canRead()) {
        LOG("[ancs] GAP Device Name characteristic not readable\n");
        return;
    }

    NimBLEAttValue val = name_chr->readValue();
    size_t n = val.size();
    if (n >= sizeof(g_phone_name)) n = sizeof(g_phone_name) - 1;
    memcpy(g_phone_name, val.data(), n);
    g_phone_name[n] = '\0';
    LOG("[ancs] phone name: '%s'\n", g_phone_name);
}

// ── iPhone time (Current Time Service) — SECONDARY/backup clock source ─────
//
// Orion is PRIMARY (Time Sync gives real UTC + IANA tz). iOS also exposes the
// standard Current Time Service (0x1805 / Current Time 0x2A2B) to bonded peers —
// the same way it exposes the GAP Device Name we already read — so the iPhone
// can seed the clock when Orion is absent (e.g. a cold power cycle with Orion
// not running). On connect we only set the clock if Orion hasn't yet (force=false).
// poll() re-reads every CTS_SYNC_INTERVAL_MS when Orion is absent to correct drift.
//
// Current Time (0x2A2B) "Exact Time 256", 10 bytes, little-endian year:
//   [0..1] year  [2] month(1-12)  [3] day  [4] hours  [5] min  [6] sec
//   [7] day-of-week  [8] fractions256  [9] adjust-reason

constexpr uint32_t CTS_SYNC_INTERVAL_MS = 10UL * 60 * 1000; // 10 minutes
static uint32_t s_last_cts_ms = 0; // millis() of last successful CTS read

// ── iPhone link signal (RSSI) ────────────────────────────────────────────
// Polled periodically rather than on every tick — ble_gap_conn_rssi() is an
// HCI round-trip, and the signal bar only needs to feel "live", not track
// every single connection event.
constexpr uint32_t RSSI_POLL_INTERVAL_MS = 5000; // 5 s
static uint32_t s_last_rssi_poll_ms = 0;
static uint8_t  g_signal_bars = 0;

// ── Deferred ANCS UI refresh ─────────────────────────────────────────────
// Set by queue_add()/queue_remove() (any live change to who's in the queue —
// filter changes go through set_filter() and call modal_ancs_list::
// refresh_active() / modal_iphone_info::refresh_active() directly instead,
// see below), drained once per poll(). Drives BOTH the drill-down list and
// the iPhone Info modal's bubble counts — see the drain site in poll().
//
// Deferred rather than calling modal_ancs_list::refresh_active() straight
// from queue_add()/queue_remove(): those two functions are shared by every
// caller that mutates the queue, including modal_ancs_list's OWN
// swipe-to-delete gesture (on_row_swipe_committed -> commit_row_delete ->
// dismiss_notification()/drop_notification() -> queue_remove(), all the way
// from an LVGL animation-completed callback still running inside
// lv_timer_handler()'s anim processing for THIS tick). Rebuilding the list
// synchronously from there would lv_obj_clean() out the very `row` object
// whose swipe just triggered it — and that row has a SECOND animation (the
// translate-x drag-back/off-screen one) with no completed_cb of its own,
// which LVGL may not have finished reaping yet in the same anim pass — a
// use-after-free on the next frame. Every OTHER caller of queue_add/remove
// (the iPhone's own NS/DS events, Orion's char-0012 actions relayed via
// ble_manager) reaches them from a plain function call, not a live LVGL
// callback, so an immediate refresh would be safe there — but the shared
// functions can't tell which caller they're being invoked from, so every
// path defers uniformly. main.cpp's loop() calls ble_manager::poll() (which
// calls this module's poll(), below) BEFORE lv_timer_handler() each
// iteration, so the earliest a flag set during iteration N's
// lv_timer_handler() gets drained is iteration N+1's poll() call — by
// definition after iteration N's anim processing has fully returned.
static bool g_ancs_list_refresh_pending = false;

// Buckets a live RSSI reading (dBm, more negative = weaker) into the same
// 0-4 bar scale the iPhone Info overlay and Orion's UI render. Thresholds are
// a standard-ish RSSI-to-bars ladder — BLE has no calibrated "signal
// strength" concept like cellular, so this is a reasonable approximation,
// not a spec'd value.
static uint8_t rssi_to_bars(int rssi) {
    if (rssi >= -60) return 4;
    if (rssi >= -70) return 3;
    if (rssi >= -80) return 2;
    if (rssi >= -90) return 1;
    return 0;
}

static void read_phone_time(bool force = false) {
    if (!g_client) return;
    // On connect (force=false) only seed if the clock isn't valid yet — Orion,
    // when present, is authoritative. The periodic re-read (force=true) runs only
    // while Orion is offline (poll()), keeping the clock live from the iPhone.
    if (!force && app_state::clock_is_set()) return;  // Orion already set it

    NimBLERemoteService* cts = g_client->getService(NimBLEUUID((uint16_t)0x1805));
    if (!cts) { LOG("[ancs] iPhone has no Current Time Service\n"); return; }
    NimBLERemoteCharacteristic* ct =
        cts->getCharacteristic(NimBLEUUID((uint16_t)0x2A2B));
    if (!ct || !ct->canRead()) { LOG("[ancs] Current Time char not readable\n"); return; }

    NimBLEAttValue val = ct->readValue();
    if (val.size() < 7) { LOG("[ancs] Current Time payload too short\n"); return; }
    const uint8_t* b = val.data();
    uint16_t year = (uint16_t)(b[0] | (b[1] << 8));
    if (year < 2020 || year > 2099 || b[2] < 1 || b[2] > 12 || b[3] < 1 || b[3] > 31) {
        LOG("[ancs] Current Time fields out of range\n");
        return;
    }

    struct tm tmv = {};
    tmv.tm_year = year - 1900;
    tmv.tm_mon  = b[2] - 1;
    tmv.tm_mday = b[3];
    tmv.tm_hour = b[4];
    tmv.tm_min  = b[5];
    tmv.tm_sec  = b[6];

    // CTS gives LOCAL wall time with no timezone. Which TZ we interpret it in
    // fixes the epoch basis of settimeofday():
    //  • If Orion has synced this session it established a true-UTC clock and a
    //    POSIX TZ. Reuse that TZ so mktime() maps the CTS wall time onto the SAME
    //    true-UTC basis — otherwise time(nullptr) would jump by the tz offset on
    //    every re-seed and corrupt epoch-relative state (the "last synced" pill
    //    sticks on "now", meeting expiry misfires).
    //  • On a cold boot where Orion has never synced there is no known TZ, so
    //    treat CTS as UTC0 (display-only — no meetings / sync stamp exist yet).
    //    Orion re-bases the clock the moment it connects.
    const char* tz = (app_state::orion_clock_synced() && app_state::orion_tz()[0])
                         ? app_state::orion_tz() : "UTC0";
    setenv("TZ", tz, 1);
    tzset();
    time_t epoch = mktime(&tmv);
    if (epoch <= 0) return;
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    s_last_cts_ms = millis();
    LOG("[ancs] clock synced from iPhone CTS: %04u-%02u-%02u %02u:%02u (tz=%s)\n",
        (unsigned)year, (unsigned)b[2], (unsigned)b[3], (unsigned)b[4], (unsigned)b[5], tz);
}

// ── ANCS EventFlags bits (Notification Source byte 1) ──────────────────────
namespace EvtFlag {
    constexpr uint8_t SILENT = 0x01, IMPORTANT = 0x02, PREEXISTING = 0x04,
                      POSITIVE_ACTION = 0x08, NEGATIVE_ACTION = 0x10;
}

// ── Notification filter ───────────────────────────────────────────────────
// 0x00 DISABLED, 0x01 CALL_ONLY, 0x02 IMPORTANT, 0x03 ALL (default).
// Set via Device Settings (char 000E "f" field) by Orion; persisted to NVS.
// Loaded at boot by state_machine::init() from NVS before the first iPhone
// connection so the filter is active from the very first notification.
static uint8_t g_filter = 0x03;  // default: ALL

static bool passes_filter(uint8_t cat_id, uint8_t flags) {
    switch (g_filter) {
        case 0x00: return false;
        case 0x01: return (cat_id == 1);                          // IncomingCall only
        case 0x02: return (cat_id == 1) ||                        // IncomingCall OR Important flag
                          ((flags & EvtFlag::IMPORTANT) != 0);
        default:   return true;                                    // ALL
    }
}

// Whether a STORED notification (already in app_state, so its Important flag
// is known) currently passes the filter — the one place that turns a uid into
// the (category, flags) pair passes_filter() above needs. Reused by both the
// on-device status bar (publish_queue()) and the Orion relay (relay_ancs_add(),
// ble-protocol.md §13's "SAME filter evaluation... not a second implementation")
// so a filter change behaves identically on both surfaces.
static bool passes_current_filter(uint32_t uid, uint8_t cat) {
    bool important = app_state::ancs_is_important(uid);
    return passes_filter(cat, important ? EvtFlag::IMPORTANT : 0);
}

// ── Pending NS-event metadata ──────────────────────────────────────────────
// EventFlags + CategoryID live in the NS event but aren't repeated in the DS
// attribute response, so stash them by UID until the DS response arrives.
// Sized to MAX_ANCS_NOTIFICATIONS: on a backlog-replay reconnect, every NS
// "Added" event for the whole backlog is processed (and calls pmeta_put) before
// any DS response is parsed (DS take only happens once GetNotificationAttributes
// actually answers, which lags the burst of CP requests) — so the number of
// simultaneously in-flight entries can reach the full backlog size, not just a
// handful. Used to be 8, which silently clobbered slot 0 on the 9th+ pending
// notification, corrupting that (and the previous occupant's) flags/category
// to defaults — symptom: a notification's real ANCS negative action silently
// turns into a generic "Close" button because PerformNotificationAction's
// NEGATIVE_ACTION flag got reset to 0 along with everything else.
struct PendingMeta { uint32_t uid; uint8_t flags; uint8_t cat; bool used; };
static PendingMeta g_pmeta[app_state::MAX_ANCS_NOTIFICATIONS];

static void pmeta_put(uint32_t uid, uint8_t flags, uint8_t cat) {
    PendingMeta* slot = nullptr;
    for (auto& m : g_pmeta) if (m.used && m.uid == uid) { slot = &m; break; }
    if (!slot) for (auto& m : g_pmeta) if (!m.used) { slot = &m; break; }
    if (!slot) slot = &g_pmeta[0];   // all in flight (unlikely) — reuse slot 0
    slot->used = true; slot->uid = uid; slot->flags = flags; slot->cat = cat;
}

static bool pmeta_take(uint32_t uid, uint8_t* flags, uint8_t* cat) {
    for (auto& m : g_pmeta) {
        if (m.used && m.uid == uid) {
            if (flags) *flags = m.flags;
            if (cat)   *cat   = m.cat;
            m.used = false;
            return true;
        }
    }
    return false;
}

// ── App display-name cache (bundle id → localized name) ────────────────────
// Filled by GetAppAttributes responses so we resolve real names for apps not
// in the built-in bundle map, once per app.
struct AppNameEntry { char bundle[48]; char name[40]; bool used; };
static AppNameEntry g_appnames[16];

static const char* appname_cache_lookup(const char* bundle) {
    if (!bundle) return nullptr;
    for (auto& e : g_appnames) if (e.used && strcmp(e.bundle, bundle) == 0) return e.name;
    return nullptr;
}

static void appname_cache_put(const char* bundle, const char* name) {
    if (!bundle || !bundle[0] || !name || !name[0]) return;
    AppNameEntry* slot = nullptr;
    for (auto& e : g_appnames) if (e.used && strcmp(e.bundle, bundle) == 0) { slot = &e; break; }
    if (!slot) for (auto& e : g_appnames) if (!e.used) { slot = &e; break; }
    if (!slot) slot = &g_appnames[0];   // cache full — evict slot 0
    slot->used = true;
    snprintf(slot->bundle, sizeof(slot->bundle), "%s", bundle);
    snprintf(slot->name,   sizeof(slot->name),   "%s", name);
}

// GetAppAttributes (CommandID 0x01): ask the phone for an app's localized
// Display Name. Used for apps not in the built-in bundle map; the async
// response lands in on_data_source and back-fills the name.
static void request_app_attributes(const char* bundle) {
    if (!g_cp_char || !bundle || !bundle[0]) return;
    uint8_t cmd[64];
    size_t  n = 0;
    cmd[n++] = 0x01;                                  // GetAppAttributes
    size_t blen = strlen(bundle);
    if (blen > sizeof(cmd) - 3) blen = sizeof(cmd) - 3;
    memcpy(&cmd[n], bundle, blen); n += blen;
    cmd[n++] = 0x00;                                  // null-terminate AppIdentifier
    cmd[n++] = 0x00;                                  // AttributeID 0 = Display Name (no length)
    g_cp_char->writeValue(cmd, n, true);
    LOG("[ancs] GetAppAttributes bundle=%s\n", bundle);
}

// ─────────────────────────────────────────────────────────────────────────
// Queue helpers
// ─────────────────────────────────────────────────────────────────────────

// Mirror the live queue into app_state, deduplicating into status-bar slots so
// multiple notifications stack into one icon.
//   • Grouping key is (icon token, title) — NOT app alone. Two different
//     Messenger senders are two different titles, so they get two separate
//     bubbles; only repeated notifications sharing the same app AND title
//     (e.g. several messages from the same sender) stack into one slot's
//     count badge. Title comes from app_state::ancs_title(), so a queue entry
//     whose attributes haven't arrived yet (title still "") groups with other
//     not-yet-resolved entries from the same app until it does.
//   • Unknown apps (token "unknown") have no brand icon and render a per-category
//     fallback glyph, so grouping them all by token would collapse unrelated
//     apps into a single bell. They additionally require matching ANCS
//     CATEGORY, so e.g. unknown email apps never group with unknown social
//     apps even if (implausibly) titles collided. `cat` is only significant
//     for the "unknown" token.
//
// Groups are ordered oldest→newest by each group's most recent ACTUAL
// notification time (ANCS Date attribute, via app_state::ancs_recv_epoch),
// so the rightmost status-bar icon is the genuinely most recent notification —
// NOT simply whichever group has the highest g_queue index. Queue index alone
// would be wrong whenever a `Modified` event refreshes an existing UID's
// content without moving its queue position (queue_add() no-ops on a UID
// already present), which would otherwise leave a just-updated notification
// stuck wherever it first arrived. Entries with no known timestamp (clock not
// yet synced, or the phone omitted Date) fall back to queue position.
static void publish_queue() {
    app_state::AncsConfig cfg = {};
    cfg.phone_connected = true;

    struct Slot {
        const char* token;
        const char* title;        // group identity, with token (+ cat for "unknown")
        uint32_t    uid;          // representative — most recent by real time
        uint8_t     count;
        uint8_t     cat;
        time_t      sort_epoch;   // representative's recv_epoch (0 = unknown)
        int         sort_idx;     // queue index — tiebreak / fallback for epoch 0
    };
    Slot slots[app_state::MAX_ANCS_NOTIFICATIONS] = {};
    size_t slot_count = 0;

    for (int i = 0; i < (int)g_queue_count; ++i) {
        const char* tok        = g_queue[i].icon_token;
        uint32_t    uid        = g_queue[i].uid;
        bool        is_unknown = (strcmp(tok, "unknown") == 0);
        uint8_t     cat        = app_state::ancs_category(uid);
        // Display-time filter: entries are always stored but only shown when they
        // pass the current filter level, so a filter change takes effect instantly.
        if (!passes_current_filter(uid, cat)) continue;
        time_t      epoch      = app_state::ancs_recv_epoch(uid);
        const char* title      = app_state::ancs_title(uid);

        Slot* slot = nullptr;
        for (size_t j = 0; j < slot_count; ++j) {
            if (strcmp(slots[j].token, tok) == 0 && (!is_unknown || slots[j].cat == cat) &&
                strcmp(slots[j].title, title) == 0) {
                slot = &slots[j];
                break;
            }
        }
        if (!slot) {
            if (slot_count >= app_state::MAX_ANCS_NOTIFICATIONS) continue;
            slot = &slots[slot_count++];
            slot->token = tok;
            slot->title = title;
            slot->cat   = cat;
            slot->count = 0;
            slot->uid = uid; slot->sort_epoch = epoch; slot->sort_idx = i;
        }
        slot->count++;
        bool newer = (epoch > 0 && slot->sort_epoch > 0) ? (epoch >= slot->sort_epoch)
                                                          : (i >= slot->sort_idx);
        if (newer) { slot->uid = uid; slot->sort_epoch = epoch; slot->sort_idx = i; }
    }

    // Stable insertion sort, oldest→newest by representative time (slot_count
    // is small — ≤ a handful of distinct apps in practice).
    for (size_t a = 1; a < slot_count; ++a) {
        Slot key = slots[a];
        size_t b = a;
        while (b > 0) {
            const Slot& prev = slots[b - 1];
            bool prev_after_key = (prev.sort_epoch > 0 && key.sort_epoch > 0)
                                   ? (prev.sort_epoch > key.sort_epoch)
                                   : (prev.sort_idx > key.sort_idx);
            if (!prev_after_key) break;
            slots[b] = slots[b - 1];
            --b;
        }
        slots[b] = key;
    }

    cfg.count = slot_count;
    for (size_t i = 0; i < slot_count; ++i) {
        cfg.icons[i]  = slots[i].token;
        cfg.uids[i]   = slots[i].uid;
        cfg.counts[i] = slots[i].count;
    }

    app_state::set_ancs_config(cfg);
    widget_status_bar::refresh_active();
}

// Counts the live queue into the three mutually-exclusive stat buckets:
// `missed` (CategoryID MissedCall), `unread` (Social), `other` (everything
// else NOT already counted under missed/unread — otherwise a missed call
// would be counted twice, once under its own badge and once under the bell).
// The ONE counting rule for every surface that shows these numbers — Ori's
// own iPhone Info tiles (phone_stats), Orion's relayed copy
// (push_phone_stats → char 000F), and the drill-down lists behind both
// (list_bucket_groups / Orion's char-0010 mirror). Two deliberate exclusions:
//   • Filter-gated: only entries that pass passes_current_filter() count —
//     same gate as relay_ancs_add()/publish_queue() (ble-protocol.md §13's
//     "SAME filter evaluation, not a second implementation"), so a badge can
//     never disagree with the list a tap on it opens. (Policy changed
//     2026-07-11: the on-device counts/list previously bypassed the filter
//     on purpose — connectivity.md's old "drill-down bypasses the ambient
//     filter" rule — which left them disagreeing with Orion's filtered view.)
//   • Ringing/active calls are skipped — they have their own live UI
//     (modal_incoming_call / AncsCallState char 0011) and never appear as a
//     list row, so counting them would inflate the bell badge over an empty
//     list.
static void count_filtered_stats(uint8_t& missed, uint8_t& unread, uint8_t& other) {
    missed = unread = other = 0;
    for (size_t i = 0; i < g_queue_count; ++i) {
        uint32_t uid = g_queue[i].uid;
        uint8_t  cat = app_state::ancs_category(uid);
        if (cat == app_state::AncsCategory::INCOMING_CALL ||
            cat == app_state::AncsCategory::ACTIVE_CALL) {
            continue;
        }
        if (!passes_current_filter(uid, cat)) continue;
        if (cat == app_state::AncsCategory::MISSED_CALL) ++missed;
        else if (cat == app_state::AncsCategory::SOCIAL) ++unread;
        else ++other;
    }
}

// Relays the filtered stat counts to Orion via the Phone Bond Status
// characteristic (char 000F). Called after every queue_add/queue_remove,
// from the RSSI poll, and from set_filter() (so Orion's badge updates the
// instant the filter changes).
static void push_phone_stats() {
    uint8_t missed, unread, other;
    count_filtered_stats(missed, unread, other);
    gatt_server::notify_phone_stats(missed, unread, other, g_signal_bars);
}

// ── ANCS relay to Orion (chars 0010/0011, ble-protocol.md §13) ────────────

// Relay a single "add" to Orion (char 0010). Called unconditionally from
// on_data_source() after every GetNotificationAttributes response — covers
// both a genuinely-new notification and iOS sending a Modified event for one
// already queued (Orion replaces its stored copy in place, keyed by uid) —
// and again from set_filter()'s clear-and-repopulate when the filter changes.
// Applies the SAME filter test as the on-device status bar (publish_queue,
// below) — not a second filter implementation. Calls (IncomingCall/
// ActiveCall) never ride this characteristic — they relay exclusively via
// AncsCallState (char 0011) at their own call sites in on_data_source() /
// on_notification_source() / on_iphone_disconnected().
static void relay_ancs_add(uint32_t uid, const char* token) {
    uint8_t cat = app_state::ancs_category(uid);
    if (cat == app_state::AncsCategory::INCOMING_CALL ||
        cat == app_state::AncsCategory::ACTIVE_CALL) {
        return;
    }
    if (!passes_current_filter(uid, cat)) return;

    const app_state::AncsNotification& n = app_state::ancs_notification_by_uid(uid);
    gatt_server::notify_ancs_add(uid, token, cat, n.display_name, n.title, n.body,
                                  (uint32_t)app_state::ancs_recv_epoch(uid),
                                  n.pos_label, n.neg_label, n.has_neg_action, n.silent);
}

// Cache of the most recently relayed call state (char 0011) — every call
// site that pushes AncsCallState to Orion goes through relay_call_state()
// below instead of calling gatt_server::notify_ancs_call_state() directly,
// so this stays current and ancs_client::resync_orion_call_state() can
// replay it on a fresh Orion connect. Same structural gap as chars 0010
// (relay_ancs_add()'s own doc comment / resync_orion_relay()): char 0011 is
// notify-only with no read property and no replay-on-reconnect of its own.
// Without this, a call already ringing or active BEFORE Orion connects (app
// just launched, or the BLE link happened to be down when the call started)
// would leave Orion showing "no call" until the call's NEXT transition —
// which for an already-active call might be "it ends," too late to ever
// show the in-call view for it at all.
struct CallStateCache {
    uint8_t  st = 0;   // 0=none/ended, 1=ringing, 2=active
    uint32_t uid = 0;
    char     app[65]        = "";
    char     title[193]     = "";
    char     pos_label[33]  = "";
    char     neg_label[33]  = "";
    bool     has_neg_action = false;
    char     icon_token[25] = "";  // calling app's icon token (ancs_icons.h vocab,
                                    // ble-protocol.md §13) — lets Orion show the
                                    // real app icon (Viber/Phone/…) for the call,
                                    // same as it does for ordinary notifications.
};
static CallStateCache g_call_state;

static void relay_call_state(uint8_t st, uint32_t uid, uint32_t elapsed_s,
                              const char* app = nullptr, const char* title = nullptr,
                              const char* pos_label = nullptr, const char* neg_label = nullptr,
                              bool has_neg_action = false, const char* icon_token = nullptr) {
    g_call_state.st  = st;
    g_call_state.uid = uid;
    strncpy(g_call_state.app, app ? app : "", sizeof(g_call_state.app) - 1);
    g_call_state.app[sizeof(g_call_state.app) - 1] = '\0';
    strncpy(g_call_state.title, title ? title : "", sizeof(g_call_state.title) - 1);
    g_call_state.title[sizeof(g_call_state.title) - 1] = '\0';
    strncpy(g_call_state.pos_label, pos_label ? pos_label : "", sizeof(g_call_state.pos_label) - 1);
    g_call_state.pos_label[sizeof(g_call_state.pos_label) - 1] = '\0';
    strncpy(g_call_state.neg_label, neg_label ? neg_label : "", sizeof(g_call_state.neg_label) - 1);
    g_call_state.neg_label[sizeof(g_call_state.neg_label) - 1] = '\0';
    strncpy(g_call_state.icon_token, icon_token ? icon_token : "", sizeof(g_call_state.icon_token) - 1);
    g_call_state.icon_token[sizeof(g_call_state.icon_token) - 1] = '\0';
    g_call_state.has_neg_action = has_neg_action;
    gatt_server::notify_ancs_call_state(st, uid, elapsed_s, app, title, pos_label, neg_label,
                                        has_neg_action, icon_token);
}

static void queue_add(uint32_t uid, const char* token) {
    // Check for duplicate UID.
    for (size_t i = 0; i < g_queue_count; ++i) {
        if (g_queue[i].uid == uid) return;
    }

    if (g_queue_count >= app_state::MAX_ANCS_NOTIFICATIONS) {
        // Displace oldest (FIFO). Relay its removal to Orion first (char
        // 0010, while it's still actually in g_queue/has stored detail) —
        // ble-protocol.md §13 explicitly calls out FIFO eviction as a
        // "leaves Ori's queue" trigger; this path doesn't go through
        // queue_remove() below, so it needs its own relay call.
        uint32_t evicted_uid = g_queue[0].uid;
        uint8_t  evicted_cat = app_state::ancs_category(evicted_uid);
        if (evicted_cat != app_state::AncsCategory::INCOMING_CALL &&
            evicted_cat != app_state::AncsCategory::ACTIVE_CALL) {
            gatt_server::notify_ancs_remove(evicted_uid);
        }
        memmove(&g_queue[0], &g_queue[1],
                (app_state::MAX_ANCS_NOTIFICATIONS - 1) * sizeof(ancs_client::QueueEntry));
        g_queue_count = app_state::MAX_ANCS_NOTIFICATIONS - 1;
    }

    g_queue[g_queue_count].uid = uid;
    strncpy(g_queue[g_queue_count].icon_token,
            token ? token : "unknown",
            sizeof(g_queue[g_queue_count].icon_token) - 1);
    g_queue[g_queue_count].icon_token[sizeof(g_queue[g_queue_count].icon_token) - 1] = '\0';
    g_queue_count++;

    // Sync icon state to app_state so the status bar widget refreshes.
    publish_queue();
    push_phone_stats();
    g_ancs_list_refresh_pending = true;  // drained by poll() — see its doc comment

    state_machine::set_phone_connected(true);
    LOG("[ancs] queued uid=%u token=%s count=%u\n",
                   (unsigned)uid, token ? token : "?", (unsigned)g_queue_count);
}

static void queue_remove(uint32_t uid) {
    for (size_t i = 0; i < g_queue_count; ++i) {
        if (g_queue[i].uid == uid) {
            // Relay this notification leaving the queue to Orion, while its
            // detail (and thus category) is still present. This is the SINGLE
            // choke-point for "left the queue → tell Orion", split by kind:
            //   - non-call → AncsNotification{op:"remove"} (char 0010)
            //   - call     → AncsCallState{st:0} (char 0011)
            // Both are unconditional per ble-protocol.md §13 ("no need to
            // check whether it was previously relayed"; a stray remove/end is
            // a harmless no-op on Orion's side).
            //
            // The call-state st:0 MUST live here, not in on_notification_source's
            // Removed handler, because a dismiss/decline/end initiated from
            // Orion (or Ori's own screen) runs dismiss_notification() →
            // queue_remove() FIRST, dropping the stored detail — so by the time
            // the phone's own ANCS Removed event arrives, ancs_category(uid)
            // there reads a defaulted OTHER and the call check misfires,
            // leaving Orion's incoming/in-call modal open forever with no st:0
            // to close it. Reading the category here (still live) and firing
            // from whichever removal path hits first fixes that; the second
            // path finds the uid already gone and no-ops (this loop won't match).
            uint8_t cat = app_state::ancs_category(uid);
            if (cat == app_state::AncsCategory::INCOMING_CALL ||
                cat == app_state::AncsCategory::ACTIVE_CALL) {
                relay_call_state(0, uid, 0);
            } else {
                gatt_server::notify_ancs_remove(uid);
            }

            app_state::remove_ancs_detail(uid);
            memmove(&g_queue[i], &g_queue[i + 1],
                    (g_queue_count - i - 1) * sizeof(ancs_client::QueueEntry));
            g_queue_count--;
            publish_queue();
            push_phone_stats();
            g_ancs_list_refresh_pending = true;  // drained by poll() — see its doc comment
            LOG("[ancs] removed uid=%u count=%u\n",
                           (unsigned)uid, (unsigned)g_queue_count);
            return;
        }
    }
    // Not found (already removed, or never queued) — still drop any stored
    // detail for a hidden (beyond-the-5th) entry, matching prior behavior.
    app_state::remove_ancs_detail(uid);
}

// ─────────────────────────────────────────────────────────────────────────
// NS callback (notification events)
// ─────────────────────────────────────────────────────────────────────────

// Forward declare
static void notify_ns_cb(NimBLERemoteCharacteristic* c,
                          uint8_t* data, size_t len, bool is_notify);
static void notify_ds_cb(NimBLERemoteCharacteristic* c,
                          uint8_t* data, size_t len, bool is_notify);

} // namespace

namespace ancs_client {

void init() {
    LOG("[ancs] client init\n");
    // NimBLE client profile registration happens in on_iphone_connected().
}

void poll(bool orion_connected) {
    // Periodic iPhone CTS re-read — corrects clock drift when Orion is absent.
    // Only runs when the iPhone is connected and Orion is not; Orion's own
    // Time Sync (every 10 min) takes priority when it is connected.
    if (!orion_connected && g_client) {
        uint32_t now = millis();
        if (s_last_cts_ms == 0 || (now - s_last_cts_ms) >= CTS_SYNC_INTERVAL_MS) {
            read_phone_time(/*force=*/true);
        }
    }

    // Periodic RSSI poll — feeds the iPhone Info overlay's signal bars and
    // Orion's relayed copy. getRssi() is a live HCI round-trip
    // (ble_gap_conn_rssi on the actual connection, not a stale scan-time
    // value), so this doesn't need to run every tick.
    if (g_client) {
        uint32_t now = millis();
        if (s_last_rssi_poll_ms == 0 || (now - s_last_rssi_poll_ms) >= RSSI_POLL_INTERVAL_MS) {
            s_last_rssi_poll_ms = now;
            uint8_t bars = rssi_to_bars(g_client->getRssi());
            if (bars != g_signal_bars) {
                g_signal_bars = bars;
                push_phone_stats();
                // Recolour an already-open iPhone Info modal's bars too —
                // previously only Orion heard about a bar change; this modal
                // sat stale for up to its whole time on screen otherwise.
                modal_iphone_info::set_signal_bars(g_signal_bars);
            }
        }
    }

    // Drain NS/DS notifications captured by the host-task callbacks. Runs on the
    // main loop task, so on_notification_source()'s blocking CP write and the
    // queue_add/remove LVGL refresh are both safe here. Merge-drain the two
    // rings by sequence number so events are processed in true arrival order
    // even though NS and DS now queue separately (see struct comment above).
    NsRaw ns; bool have_ns = ns_pop(ns);
    DsRaw ds; bool have_ds = ds_pop(ds);
    while (have_ns || have_ds) {
        bool take_ns = have_ns && (!have_ds || ns.seq < ds.seq);
        if (take_ns) {
            on_notification_source(ns.buf, ns.len);
            have_ns = ns_pop(ns);
        } else {
            on_data_source(ds.buf, ds.len);
            have_ds = ds_pop(ds);
        }
    }

    // Drain the deferred UI refresh — see g_ancs_list_refresh_pending's doc
    // comment for why this can't be called straight from queue_add()/
    // queue_remove(). Both calls are no-ops when their modal isn't open.
    //
    // modal_iphone_info::refresh_active() belongs here, not only in
    // set_filter(): the iPhone Info modal's bubble counts (missed/messages/
    // notifications) are derived from phone_stats(), which changes on every
    // queue add/remove — so without this, a notification added or removed by
    // ANY path (the iPhone's own ANCS events, Ori's swipe-to-delete, or an
    // Orion-relayed dismiss over char 0012) left the tiles stale until the
    // modal was closed and reopened. push_phone_stats() already keeps Orion's
    // relayed copy live; this keeps Ori's own tiles equally live.
    if (g_ancs_list_refresh_pending) {
        g_ancs_list_refresh_pending = false;
        modal_ancs_list::refresh_active();
        modal_iphone_info::refresh_active();
    }
}

void on_iphone_connected(uint16_t conn_handle) {
    // Idempotent: a second dispatch for the same live connection (e.g. both
    // the fresh-bond and reconnect paths firing) must not re-run discovery
    // or double-subscribe.
    if (conn_handle == g_conn_handle && g_ns_char) return;

    LOG("[ancs] iPhone connected, handle=%u\n", (unsigned)conn_handle);
    g_conn_handle = conn_handle;

    // In NimBLE 2.5, the server hosts the ANCS client connection.
    // We use the server's getClient() method.
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server) {
        g_client = server->getClient(conn_handle);
    }
    if (!g_client) {
        LOG("[ancs] WARN: no NimBLEClient for ANCS handle (expected for server-only builds)\n");
        // ANCS uses the remote service discovery API from a NimBLEClient perspective.
        // On ESP32-S3 with both peripheral+central roles enabled, this will work.
        // If central role is not compiled in, ANCS discovery will be skipped gracefully.
        return;
    }

    // Discover ANCS service and characteristics.
    g_ancs_svc = g_client->getService(ANCS_SVC_UUID);
    if (!g_ancs_svc) {
        LOG("[ancs] ANCS service not found — is Bluetooth permission granted?\n");
        return;
    }

    g_ns_char = g_ancs_svc->getCharacteristic(ANCS_NS_UUID);
    g_cp_char = g_ancs_svc->getCharacteristic(ANCS_CP_UUID);
    g_ds_char = g_ancs_svc->getCharacteristic(ANCS_DS_UUID);

    if (!g_ns_char || !g_cp_char || !g_ds_char) {
        LOG("[ancs] missing NS/CP/DS characteristics\n");
        return;
    }

    // Subscribe to Notification Source. Write-with-response (the subscribe()
    // default) so a failed CCCD write — e.g. if the link somehow isn't yet
    // encrypted — is reported instead of silently dropped. The previous code
    // passed response=false and logged "subscribed" unconditionally, which hid
    // failures. iOS only streams notifications once NS is actually subscribed.
    if (g_ns_char->canNotify()) {
        bool ok = g_ns_char->subscribe(true, notify_ns_cb);
        LOG("[ancs] subscribe NS: %s\n", ok ? "ok" : "FAILED");
    } else {
        LOG("[ancs] NS characteristic cannot notify\n");
    }

    // Subscribe to Data Source.
    if (g_ds_char->canNotify()) {
        bool ok = g_ds_char->subscribe(true, notify_ds_cb);
        LOG("[ancs] subscribe DS: %s\n", ok ? "ok" : "FAILED");
    } else {
        LOG("[ancs] DS characteristic cannot notify\n");
    }

    // Fetch the phone's device name for display (unpair modal).
    read_phone_name();

    // Seed the clock from the iPhone if Orion hasn't (secondary time source).
    read_phone_time();

    state_machine::set_phone_connected(true);
}

void on_iphone_disconnected() {
    LOG("[ancs] iPhone disconnected\n");

    // A call can't survive the ANCS link dropping — close any open call
    // banner/dialog and stop the duration timer before clearing state, and
    // relay the same "ended" state to Orion (char 0011) — unconditionally,
    // mirroring close_all()'s own "regardless of UID" style; a harmless
    // no-op on Orion's side if no call view was actually open.
    modal_incoming_call::close_all();
    relay_call_state(0, 0, 0);

    g_conn_handle   = 0xFFFF;
    g_client        = nullptr;
    g_ancs_svc      = nullptr;
    g_ns_char       = nullptr;
    g_cp_char       = nullptr;
    g_ds_char       = nullptr;
    g_phone_name[0] = '\0';
    s_last_cts_ms   = 0;
    s_last_rssi_poll_ms = 0;
    g_signal_bars   = 0;

    // Clear queue and update status bar.
    g_queue_count = 0;
    app_state::AncsConfig cfg = {};
    cfg.phone_connected = false;
    cfg.count = 0;
    app_state::set_ancs_config(cfg);
    widget_status_bar::refresh_active();

    // The queue was just zeroed directly above (not via queue_remove(), which
    // is what normally sets g_ancs_list_refresh_pending) — an open drill-down
    // list needs its own explicit nudge here or it would keep showing
    // whatever rows it had right up until the modal is closed and reopened.
    // Safe to call immediately (not deferred): this whole function is reached
    // from ble_manager::poll()'s event drain, never from inside an LVGL
    // callback — same reasoning as set_filter()'s immediate call.
    modal_ancs_list::refresh_active();

    state_machine::set_phone_connected(false);
}

void on_notification_source(const uint8_t* data, uint16_t len) {
    if (len < 8) return;

    uint8_t  event_id = data[0];
    uint8_t  flags    = data[1];   // EventFlags (Silent/Important/PreExisting/…)
    uint8_t  cat_id   = data[2];   // CategoryID (IncomingCall, Social, Email, …)
    uint32_t uid      = (uint32_t)(data[4] | (data[5] << 8) |
                                    (data[6] << 16) | (data[7] << 24));

    // Diagnostic: prove whether iOS is actually streaming NS events. If this
    // never logs while the phone has notifications, the problem is iOS-side
    // (notification access not granted / device not engaged), not the parser.
    LOG("[ancs] NS event id=%u cat=%u flags=0x%02X uid=%u\n",
        (unsigned)event_id, (unsigned)cat_id, (unsigned)flags, (unsigned)uid);

    if (event_id == 0 || event_id == 1) {
        // Added or Modified — always store, regardless of filter level. The filter
        // is applied at display time (publish_queue, note_new_notification, call modal)
        // so a filter change immediately shows/hides existing notifications without
        // needing to refetch from the iPhone. Removed events (event_id 2) are always
        // processed so dismissals work correctly regardless of filter state.
        pmeta_put(uid, flags, cat_id);
        ancs_client::request_attributes(uid);
    } else if (event_id == 2) {
        // Removed on the iPhone — close any open overlay / call banner showing
        // it, then drop it from Ori's queue + detail + status bar. The Orion
        // relay for this removal (AncsNotification{op:"remove"} for a normal
        // notification, or AncsCallState{st:0} for a call) is emitted by
        // queue_remove() itself — the single choke-point that owns it, so a
        // removal reaches Orion identically whether it originated here (phone-
        // side removed), from a local dismiss, or from an Orion-relayed action.
        // See queue_remove()'s doc comment for why the call st:0 has to live
        // there and not here.
        modal_ancs_notification::close_if_showing(uid);
        modal_incoming_call::close_if_showing(uid);
        queue_remove(uid);
    }
}

void on_data_source(const uint8_t* data, uint16_t len) {
    if (len == 0) return;

    // Accumulate DS data.
    size_t copy_len = len;
    if (g_ds_len + copy_len > sizeof(g_ds_buf)) {
        copy_len = sizeof(g_ds_buf) - g_ds_len;
    }
    memcpy(g_ds_buf + g_ds_len, data, copy_len);
    g_ds_len += copy_len;

    if (g_ds_len < 5) return;
    uint8_t cmd_id = g_ds_buf[0];

    // ── GetAppAttributes response (CommandID 0x01) — app display name ──
    if (cmd_id == 0x01) {
        // [0]=0x01, [1..]=AppIdentifier (null-terminated), then attr records.
        size_t p = 1, ai = 0;
        char app_id[128] = {};
        while (p < g_ds_len && g_ds_buf[p] != 0x00) {
            if (ai < sizeof(app_id) - 1) app_id[ai++] = (char)g_ds_buf[p];
            ++p;
        }
        if (p >= g_ds_len) return;            // AppIdentifier not fully arrived yet
        app_id[ai] = '\0';
        ++p;                                   // skip the null terminator
        char name[64] = {};
        while (p + 3 <= g_ds_len) {
            uint8_t  aid  = g_ds_buf[p];
            uint16_t alen = (uint16_t)(g_ds_buf[p + 1] | (g_ds_buf[p + 2] << 8));
            p += 3;
            if (p + alen > g_ds_len) break;
            if (aid == 0x00) {                 // Display Name
                size_t c = alen < sizeof(name) - 1 ? alen : sizeof(name) - 1;
                memcpy(name, &g_ds_buf[p], c);
                name[c] = '\0';
            }
            p += alen;
        }
        if (name[0]) {
            char fname[40] = {};
            ui::sanitize_text(name, fname, sizeof(fname));
            appname_cache_put(app_id, fname);
            app_state::set_ancs_display_name_for_bundle(app_id, fname);
            LOG("[ancs] app name: %s -> '%s'\n", app_id, fname);
        }
        g_ds_len = 0;
        return;
    }

    // ── GetNotificationAttributes response (CommandID 0x00) ──
    //   [1-4] NotificationUID (LE), then records: [AttributeID][Length LE][Data]
    if (cmd_id != 0x00) { g_ds_len = 0; return; }

    uint32_t resp_uid = (uint32_t)(g_ds_buf[1] | (g_ds_buf[2] << 8) |
                                    (g_ds_buf[3] << 16) | (g_ds_buf[4] << 24));

    // Requested: AppIdentifier(0), Title(1), Subtitle(2), Message(3), Date(5),
    //            PositiveActionLabel(6), NegativeActionLabel(7) — in that exact
    //            order (request_attributes() above), and ANCS returns attributes
    //            in the order they were requested. NegativeActionLabel(7) is
    //            therefore the LAST record in a genuinely complete response.
    char app_id[128]    = {};
    char title[193]     = {};
    char subtitle[129]  = {};
    char body[513]      = {};
    char date[24]       = {};
    char pos_label[33]  = {};
    char neg_label[33]  = {};
    bool got_app_id     = false;
    bool got_last_attr  = false;

    size_t pos = 5;
    while (pos + 3 <= g_ds_len) {
        uint8_t  attr_id  = g_ds_buf[pos];
        uint16_t attr_len = (uint16_t)(g_ds_buf[pos + 1] | (g_ds_buf[pos + 2] << 8));
        pos += 3;
        if (pos + attr_len > g_ds_len) break; // incomplete — wait for more

        char*  dst = nullptr;
        size_t dst_sz = 0;
        if      (attr_id == 0x00) { dst = app_id;    dst_sz = sizeof(app_id)    - 1; got_app_id = true; }
        else if (attr_id == 0x01) { dst = title;     dst_sz = sizeof(title)     - 1; }
        else if (attr_id == 0x02) { dst = subtitle;  dst_sz = sizeof(subtitle)  - 1; }
        else if (attr_id == 0x03) { dst = body;      dst_sz = sizeof(body)      - 1; }
        else if (attr_id == 0x05) { dst = date;      dst_sz = sizeof(date)      - 1; }
        else if (attr_id == 0x06) { dst = pos_label; dst_sz = sizeof(pos_label) - 1; }
        else if (attr_id == 0x07) { dst = neg_label; dst_sz = sizeof(neg_label) - 1; got_last_attr = true; }

        if (dst) {
            size_t copy = attr_len < dst_sz ? attr_len : dst_sz;
            memcpy(dst, &g_ds_buf[pos], copy);
            dst[copy] = '\0';
        }
        pos += attr_len;
    }

    // A fragment boundary can coincide with a complete-attribute boundary —
    // the loop above then "runs out of buffer" the exact same way it would on
    // a truly finished response, with no protocol-level way to tell those
    // apart from buffer state alone. Only trust completion once we've actually
    // parsed the LAST requested attribute; otherwise wait for the next DS
    // fragment instead of finalizing (and resetting g_ds_len) on a partial
    // response. If the phone omits the trailing attribute entirely (or drops
    // the link mid-response) this notification's attributes are silently
    // skipped — self-healing, since request_attributes() unconditionally
    // resets g_ds_len for the next notification in the queue.
    if (!got_last_attr) return;
    if (!got_app_id) { g_ds_len = 0; return; }

    // Recover the NS-event metadata stashed for this UID (flags + category).
    uint8_t flags = 0, cat = app_state::AncsCategory::OTHER;
    pmeta_take(resp_uid, &flags, &cat);
    bool silent      = (flags & EvtFlag::SILENT)            != 0;
    bool important   = (flags & EvtFlag::IMPORTANT)        != 0;
    bool preexisting = (flags & EvtFlag::PREEXISTING)      != 0;
    bool neg_action  = (flags & EvtFlag::NEGATIVE_ACTION)  != 0;

    // Resolve icon token + human-readable app name. Built-in map first (instant),
    // then the GetAppAttributes cache; if still unknown, fire a one-off fetch —
    // its async reply back-fills the name for this and future notifications.
    const BundleMap* bm = lookup_bundle(app_id);
    const char* token        = bm ? bm->token : "unknown";
    const char* display_name = bm ? bm->name  : appname_cache_lookup(app_id);
    if (!display_name) request_app_attributes(app_id);

    // Timestamp (ANCS Date attribute).
    char hhmm[6] = {};
    time_t recv_epoch = parse_ancs_date(date, hhmm, sizeof(hhmm));

    // Drop glyphs the UI font can't render (emoji, CJK, …) before storing.
    char ftitle[193] = {}, fsub[129] = {}, fbody[257] = {};
    char fpos[33] = {}, fneg[33] = {};
    ui::sanitize_text(title,     ftitle, sizeof(ftitle));
    ui::sanitize_text(subtitle,  fsub,   sizeof(fsub));
    ui::sanitize_text(body,      fbody,  sizeof(fbody));
    ui::sanitize_text(pos_label, fpos,   sizeof(fpos));
    ui::sanitize_text(neg_label, fneg,   sizeof(fneg));

    // Store detail BEFORE queue_add so the status-bar tile built during the
    // queue_add → publish_queue → refresh can read this UID's category/importance.
    app_state::set_ancs_detail(resp_uid, token, display_name, ftitle, fsub, fbody,
                               recv_epoch, hhmm, app_id, cat, important, silent,
                               fpos, fneg, neg_action);

    // Genuinely-new notifications animate in; the backlog iOS replays on connect
    // (PreExisting flag) populates silently. Only animate if the notification
    // passes the current filter — filtered-out entries are stored but not surfaced.
    if (!preexisting && passes_filter(cat, flags)) widget_status_bar::note_new_notification(resp_uid);

    // Call notification that isn't part of the reconnect backlog → call overlay.
    // Two call categories: the classic INCOMING_CALL (1, native Phone app) and
    // ACTIVE_CALL (12), which modern iOS reports for ongoing VoIP/CallKit calls
    // such as Viber. A ringing call still offers the ANCS positive (answer)
    // action; once it's answered the phone drops that action (only hang-up
    // remains) — that's how we tell "ringing" from "on call". VoIP calls often
    // arrive already in the no-positive-action state, so they open the in-call
    // dialog directly. Best-effort: relies on iOS's action flags.
    bool is_call = (cat == app_state::AncsCategory::INCOMING_CALL ||
                    cat == app_state::AncsCategory::ACTIVE_CALL);
    if (is_call && !preexisting && !ota_receiver::is_active() && passes_filter(cat, flags)) {
        bool can_answer = (flags & EvtFlag::POSITIVE_ACTION) != 0;
        if (can_answer) {
            modal_incoming_call::show(resp_uid);          // ringing
            relay_call_state(1, resp_uid, 0, display_name, ftitle, fpos, fneg, neg_action, token);
        } else {
            modal_incoming_call::notify_active(resp_uid); // on call
            // Seed "e" from the just-(re)started session so Orion's timer
            // reflects real elapsed time (relevant when this is a Modified
            // event on an already-active call rather than a fresh answer).
            uint32_t elapsed = 0;
            modal_incoming_call::session_state(nullptr, &elapsed);
            relay_call_state(2, resp_uid, elapsed, display_name, ftitle, fpos, fneg, neg_action, token);
        }
        // Refresh the status-bar tile ring (yellow ringing / red answered) for
        // this uid now — queue_add() below is a no-op for a uid already
        // queued (the ringing->active transition is usually a Modified event
        // on the SAME uid, e.g. the call was answered on the phone itself
        // rather than via Ori's own Answer button), so without this the ring
        // would silently stay yellow for the rest of the call.
        widget_status_bar::refresh_active();
    }

    queue_add(resp_uid, token);
    // Relay to Orion (char 0010) — fires for both Added and Modified (this
    // function runs once per GetNotificationAttributes response, whichever
    // triggered it); relay_ancs_add() itself excludes calls (relayed above via
    // AncsCallState instead) and applies the current ancs_filter.
    relay_ancs_add(resp_uid, token);

    g_ds_len = 0;
    g_pending_uid = 0;

    LOG("[ancs] attr uid=%u app=%s cat=%u%s title='%s'\n",
        (unsigned)resp_uid, app_id, (unsigned)cat,
        preexisting ? " (preexisting)" : "", ftitle);
}

void request_attributes(uint32_t notif_uid) {
    if (!g_cp_char) return;
    g_pending_uid = notif_uid;
    g_ds_len      = 0;

    // GetNotificationAttributes command:
    //   CommandID=0x00, UID(4B), AppID(0x00), Title(0x01,192), Subtitle(0x02,128),
    //   Message(0x03,512), Date(0x05), PositiveActionLabel(0x06,32), NegativeActionLabel(0x07,32).
    // Only variable-length string attributes carry a 2-byte max-length; AppIdentifier
    // and Date are fixed-format and take no length field (per the ANCS spec).
    uint8_t cmd[30];
    size_t  n = 0;
    cmd[n++] = 0x00; // GetNotificationAttributes
    cmd[n++] = (uint8_t)(notif_uid & 0xFF);
    cmd[n++] = (uint8_t)((notif_uid >> 8) & 0xFF);
    cmd[n++] = (uint8_t)((notif_uid >> 16) & 0xFF);
    cmd[n++] = (uint8_t)((notif_uid >> 24) & 0xFF);
    cmd[n++] = 0x00;                        // AppIdentifier (no length)
    cmd[n++] = 0x01; cmd[n++] = 192; cmd[n++] = 0;  // Title
    cmd[n++] = 0x02; cmd[n++] = 128; cmd[n++] = 0;  // Subtitle
    cmd[n++] = 0x03; cmd[n++] = 255; cmd[n++] = 1;  // Message (512 LE)
    cmd[n++] = 0x05;                        // Date (no length)
    cmd[n++] = 0x06; cmd[n++] = 32;  cmd[n++] = 0;  // PositiveActionLabel
    cmd[n++] = 0x07; cmd[n++] = 32;  cmd[n++] = 0;  // NegativeActionLabel

    g_cp_char->writeValue(cmd, n, true);
    LOG("[ancs] GetNotificationAttributes uid=%u\n", (unsigned)notif_uid);
}

void dismiss_notification(uint32_t notif_uid) {
    // Send PerformNotificationAction(Negative) to clear it on the phone.
    if (g_cp_char) {
        uint8_t cmd[6];
        cmd[0] = 0x02; // PerformNotificationAction
        cmd[1] = (uint8_t)(notif_uid & 0xFF);
        cmd[2] = (uint8_t)((notif_uid >> 8) & 0xFF);
        cmd[3] = (uint8_t)((notif_uid >> 16) & 0xFF);
        cmd[4] = (uint8_t)((notif_uid >> 24) & 0xFF);
        cmd[5] = 0x01; // ActionID Negative
        g_cp_char->writeValue(cmd, 6, true);
    }
    queue_remove(notif_uid);
}

void drop_notification(uint32_t notif_uid) {
    // Remove from Ori's queue without sending any ANCS action — used when the
    // notification has no negative action (has_neg_action = false).
    queue_remove(notif_uid);
}

void answer_notification(uint32_t notif_uid) {
    // ANCS PerformNotificationAction · Positive = answer/accept the call. Unlike
    // dismiss, the notification is NOT removed — the call becomes active and the
    // entry remains in the queue until the call ends (ANCS Removed).
    if (g_cp_char) {
        uint8_t cmd[6];
        cmd[0] = 0x02; // PerformNotificationAction
        cmd[1] = (uint8_t)(notif_uid & 0xFF);
        cmd[2] = (uint8_t)((notif_uid >> 8) & 0xFF);
        cmd[3] = (uint8_t)((notif_uid >> 16) & 0xFF);
        cmd[4] = (uint8_t)((notif_uid >> 24) & 0xFF);
        cmd[5] = 0x00; // ActionID Positive
        g_cp_char->writeValue(cmd, 6, true);
    }
}

const char* phone_name() {
    return g_phone_name;
}

PhoneStats phone_stats() {
    PhoneStats s = {};
    if (!g_client) return s;  // disconnected — all-zero, nothing to verify
    // Same filtered counting as Orion's relayed copy (count_filtered_stats)
    // — the on-device tile badges always agree with Orion's, and both always
    // match the drill-down list a tap on the tile opens.
    count_filtered_stats(s.missed, s.unread, s.total);
    s.signal_bars = g_signal_bars;
    return s;
}

// Enumerate every live notification group in `bucket` for the on-device
// drill-down list (connectivity.md §2 "Tap-to-drill-down"). Read-only — does
// not touch g_queue, publish_queue(), or push_phone_stats(). Call only from
// the main task, same as every other queue-reading function in this module.
size_t list_bucket_groups(uint8_t bucket, ListGroup* out, size_t max) {
    if (!out || max == 0) return 0;

    struct Group {
        uint32_t uid;          // representative (newest) uid in the group
        uint8_t  count;
        char     icon_token[32];
        time_t   sort_epoch;   // representative's recv_epoch (0 = unknown)
        int      sort_idx;     // g_queue index of the first-encountered member
    };
    Group  groups[app_state::MAX_ANCS_NOTIFICATIONS];
    size_t group_count = 0;

    bool visited[app_state::MAX_ANCS_NOTIFICATIONS] = {};
    for (size_t i = 0; i < g_queue_count; ++i) {
        if (visited[i]) continue;
        visited[i] = true;

        uint32_t uid = g_queue[i].uid;
        uint8_t  cat = app_state::ancs_category(uid);

        // Calls never appear in any drill-down bucket — they have their own
        // live modal_incoming_call UI, not a static list row.
        if (cat == app_state::AncsCategory::INCOMING_CALL ||
            cat == app_state::AncsCategory::ACTIVE_CALL) {
            continue;
        }

        // Resolve the full (token, title) group via the same primitive
        // modal_ancs_notification's stacking already uses, and mark every
        // member visited so it isn't processed again as the seed of its own
        // group later in this scan. Only members that pass the current
        // ancs_filter are counted and representable — the drill-down honors
        // the same filter as the status bar, the tile badges
        // (count_filtered_stats), and the Orion relay, so the row count
        // behind a badge always matches it (policy changed 2026-07-11 from
        // the earlier "drill-down bypasses the ambient filter" rule). A
        // group whose members are all filtered out produces no row.
        uint32_t tmp_uids[app_state::MAX_ANCS_NOTIFICATIONS];
        size_t n = app_state::ancs_collect_same_title(uid, tmp_uids,
                                                       app_state::MAX_ANCS_NOTIFICATIONS);
        if (n == 0) { tmp_uids[0] = uid; n = 1; }

        uint32_t    rep_uid   = 0;
        const char* rep_token = g_queue[i].icon_token;  // fallback if rep has no queue entry
        size_t      pass_n    = 0;
        for (size_t k = 0; k < n; ++k) {
            const char* tok_k = nullptr;
            for (size_t qi = 0; qi < g_queue_count; ++qi) {
                if (g_queue[qi].uid != tmp_uids[k]) continue;
                visited[qi] = true;
                tok_k = g_queue[qi].icon_token;
                break;
            }
            uint8_t mcat = app_state::ancs_category(tmp_uids[k]);
            if (mcat == app_state::AncsCategory::INCOMING_CALL ||
                mcat == app_state::AncsCategory::ACTIVE_CALL) continue;
            if (!passes_current_filter(tmp_uids[k], mcat)) continue;
            if (pass_n == 0) {
                // tmp_uids is newest-first, so the first passing member is
                // the newest one the filter allows — it becomes the row's
                // representative (title/preview/icon + detail-tap target).
                rep_uid = tmp_uids[k];
                if (tok_k) rep_token = tok_k;
            }
            ++pass_n;
        }
        if (pass_n == 0) continue;  // whole group filtered out — no row

        // Bucket from the representative (the notification the row actually
        // shows), not the scan seed — the seed itself may be filtered out.
        uint8_t rep_cat = app_state::ancs_category(rep_uid);
        uint8_t b = (rep_cat == app_state::AncsCategory::MISSED_CALL) ? ancs_client::ListBucket::MISSED
                  : (rep_cat == app_state::AncsCategory::SOCIAL)      ? ancs_client::ListBucket::UNREAD
                  :                                                     ancs_client::ListBucket::OTHER;

        if (b != bucket) continue;  // belongs to a different tile's list
        if (group_count >= app_state::MAX_ANCS_NOTIFICATIONS) continue;

        Group& g = groups[group_count++];
        g.uid   = rep_uid;
        g.count = (uint8_t)pass_n;
        strncpy(g.icon_token, rep_token, sizeof(g.icon_token) - 1);
        g.icon_token[sizeof(g.icon_token) - 1] = '\0';
        g.sort_epoch = app_state::ancs_recv_epoch(rep_uid);
        g.sort_idx   = (int)i;
    }

    // Newest-group-first — mirrors publish_queue()'s own comparator
    // (representative recv_epoch when both sides are known, else g_queue
    // arrival-index as the fallback/tiebreak), sorted in the opposite
    // direction: publish_queue builds oldest→newest (status bar renders
    // rightmost = newest); this list wants newest first.
    for (size_t a = 1; a < group_count; ++a) {
        Group key = groups[a];
        size_t b = a;
        while (b > 0) {
            const Group& prev = groups[b - 1];
            bool prev_is_older = (prev.sort_epoch > 0 && key.sort_epoch > 0)
                                     ? (prev.sort_epoch < key.sort_epoch)
                                     : (prev.sort_idx < key.sort_idx);
            if (!prev_is_older) break;
            groups[b] = groups[b - 1];
            --b;
        }
        groups[b] = key;
    }

    size_t out_n = group_count < max ? group_count : max;
    for (size_t i = 0; i < out_n; ++i) {
        out[i].uid   = groups[i].uid;
        out[i].count = groups[i].count;
        strncpy(out[i].icon_token, groups[i].icon_token, sizeof(out[i].icon_token) - 1);
        out[i].icon_token[sizeof(out[i].icon_token) - 1] = '\0';
    }
    return out_n;
}

// Full clear-and-repopulate of Orion's ANCS mirror (chars 0010/0011) —
// rather than a diff (ble-protocol.md §13), since firmware doesn't track
// which uids it previously relayed to THIS particular Orion connection:
// "clear" then re-send "add" for every currently-queued uid that passes the
// current filter (relay_ancs_add()'s own filter check does the selection;
// calls are excluded there too — chars 0010 vs 0011 are separate relays, see
// resync_orion_call_state() just below for the call-state equivalent of this
// same fix).
//
// Two callers, same reconciliation for two different reasons:
//   - set_filter(): the SET of notifications passing the filter changed.
//   - ble_manager's BleEventType::AncsResubscribed handler, fired by
//     gatt_server.cpp's onSubscribe() the instant Orion's own CCCD write for
//     char 0010 lands: this characteristic is NOTIFY-only with no read
//     property and no reconnect-replay of its own (unlike PhoneBondStatus,
//     char 000F, which Orion explicitly reads on connect — ble-protocol.md
//     §3). A notification added to g_queue while Orion was disconnected has
//     NO other path to ever reach Orion's local mirror: the original
//     relay_ancs_add() call at add-time only reaches a peer that's both
//     connected AND subscribed right then, and there's no buffering — a BLE
//     notify to a not-currently-subscribed central is simply dropped, not
//     queued. Symptom without this: PhoneBondStatus's aggregate count (read
//     fresh on every connect) shows N, but Orion's own drill-down/detail
//     modal has nothing to show for it — the count and the content silently
//     drifted apart the moment Orion was offline for even one incoming
//     notification.
//     (This used to be wired to the earlier BleEventType::OrionConnected
//     event instead — encryption complete, fired long before Orion has even
//     started run_sync, let alone subscribed to char 0010. That resync
//     compiled and looked correct but never actually landed: a notify sent
//     before the central subscribes is silently dropped by the BLE stack.
//     onSubscribe() is the first point in time this is guaranteed to work.)
void resync_orion_relay() {
    gatt_server::notify_ancs_clear();
    for (size_t i = 0; i < g_queue_count; ++i) {
        relay_ancs_add(g_queue[i].uid, g_queue[i].icon_token);
    }
}

// Replays the current call state (char 0011) to Orion the moment its own
// onSubscribe() fires for this characteristic — the AncsCallState
// equivalent of resync_orion_relay() above, for exactly the same reason
// (notify-only, no replay of its own) and triggered the same way (NOT
// OrionConnected — see resync_orion_relay()'s doc comment for why that's
// too early). Without this, a call already ringing or active BEFORE Orion
// connects (app just launched, or the BLE link happened to be down when the
// call started) left Orion showing "no call" until the call's NEXT
// transition — which for an already-active call might be "it ends," too
// late to ever raise the in-call view at all.
// Always sends, even for st==0 (nothing to show) — Orion's own default
// assumption is already "no call" so this is a harmless no-op then, and
// unconditional is simpler/more robust than trying to skip a redundant send
// (same philosophy as resync_orion_relay()'s unconditional "clear").
// Elapsed is recomputed live for an active call (not replayed from whatever
// stale value was cached when it went active) — same reasoning the original
// call site already applied when seeding a Modified event's elapsed time.
void resync_orion_call_state() {
    uint32_t elapsed = 0;
    if (g_call_state.st == 2) {
        modal_incoming_call::session_state(nullptr, &elapsed);
    }
    gatt_server::notify_ancs_call_state(g_call_state.st, g_call_state.uid, elapsed,
                                         g_call_state.app, g_call_state.title,
                                         g_call_state.pos_label, g_call_state.neg_label,
                                         g_call_state.has_neg_action, g_call_state.icon_token);
}

void set_filter(uint8_t level) {
    g_filter = level;
    LOG("[ancs] filter -> %u (%s)\n", (unsigned)level,
        level == 0 ? "Disabled" : level == 1 ? "CallOnly" :
        level == 2 ? "Important" : "All");
    // Re-publish so the status bar immediately reflects the new filter level
    // without waiting for the next incoming notification.
    publish_queue();

    resync_orion_relay();

    // Orion's PhoneBondStatus badge counts (char 000F) are filtered the same
    // way as the relay just above — push immediately so the badge updates the
    // instant the filter changes, not just on the next queue_add/queue_remove
    // or the next RSSI poll.
    push_phone_stats();

    // Ori's own iPhone Info/Stats overlay (modal_iphone_info) is otherwise a
    // snapshot, but its badge counts are the one thing that must not go
    // stale against a filter the user just changed — no-op if the modal
    // isn't currently open (modal_iphone_info.h's "ONE exception").
    modal_iphone_info::refresh_active();

    // Same for an open drill-down list (modal_ancs_list) — a filter change
    // can add or remove rows in whichever bucket is currently on screen.
    // Called directly (not deferred like queue_add/queue_remove) — set_filter()
    // is only ever reached from ble_manager::poll()'s event drain or
    // state_machine's own init path, never from inside an LVGL callback, so
    // there's no reentrancy hazard here.
    modal_ancs_list::refresh_active();
}

uint8_t get_filter() {
    return g_filter;
}

bool is_queued(uint32_t uid) {
    for (size_t i = 0; i < g_queue_count; ++i) {
        if (g_queue[i].uid == uid) return true;
    }
    return false;
}

} // namespace ancs_client

// ── NS / DS notify callbacks (NimBLE task context) ────────────────────────

namespace {

// Host-task context: ONLY copy the bytes into the ring. The actual work
// (blocking CP write, LVGL refresh) is done later by ancs_client::poll() on
// the main task — see the NS/DS deferral queue comment above.
static void notify_ns_cb(NimBLERemoteCharacteristic* /*c*/,
                          uint8_t* data, size_t len, bool /*is_notify*/) {
    ns_push(data, len);
}

static void notify_ds_cb(NimBLERemoteCharacteristic* /*c*/,
                          uint8_t* data, size_t len, bool /*is_notify*/) {
    ds_push(data, len);
}

} // namespace
