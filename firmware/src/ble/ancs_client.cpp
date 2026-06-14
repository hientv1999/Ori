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
#include "screens/modal_ancs_notification.h"
#include "screens/modal_incoming_call.h"
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
    { "com.microsoft.teams",            "teams",       "Teams"       },
    { "com.apple.MobileSMS",            "sms",         "Messages"    },
    { "com.apple.mobilephone",          "phone",       "Phone"       },
    { "com.hammerandchisel.discord",    "discord",     "Discord"     },
    { "ph.telegra.Telegraph",           "telegram",    "Telegram"    },
    { "com.google.ios.youtube",         "youtube",     "YouTube"     },
    { "com.zhiliaoapp.musically",       "tiktok",      "TikTok"      },
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

// ── Pending attributes ────────────────────────────────────────────────────

ancs_client::NotificationInfo g_pending_info;
bool                          g_pending_valid = false;

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

// ── NS/DS deferral queue (host task → main task) ──────────────────────────
//
// NimBLE invokes the NS/DS notify callbacks on the HOST task. Processing them
// there is unsafe: on_notification_source() does a blocking CP write-with-
// response (request_attributes) — which deadlocks, since the host task that
// must process the write response is the one stuck in the callback — and the
// queue_add/remove path calls into LVGL (refresh_active), which must only run
// on the main task. So the callbacks just copy the raw bytes into this single-
// producer/single-consumer ring; ancs_client::poll() drains it on the main
// task, where blocking GATT ops and LVGL are both safe.
struct AncsRaw {
    uint8_t  kind;      // 0 = Notification Source, 1 = Data Source
    uint16_t len;
    uint8_t  buf[256];  // ≥ one ATT_MTU (247) fragment
};
constexpr uint8_t ANCS_Q_SIZE = 8;
AncsRaw          g_aq[ANCS_Q_SIZE];
volatile uint8_t g_aq_head = 0;   // consumer (main task)
volatile uint8_t g_aq_tail = 0;   // producer (host task)

void aq_push(uint8_t kind, const uint8_t* data, size_t len) {
    uint8_t next = (uint8_t)((g_aq_tail + 1) % ANCS_Q_SIZE);
    if (next == g_aq_head) return;  // full → drop (next NS event re-adds it)
    AncsRaw& e = g_aq[g_aq_tail];
    e.kind = kind;
    e.len  = (len > sizeof(e.buf)) ? (uint16_t)sizeof(e.buf) : (uint16_t)len;
    memcpy(e.buf, data, e.len);
    g_aq_tail = next;               // publish only after the slot is filled
}

bool aq_pop(AncsRaw& out) {
    if (g_aq_head == g_aq_tail) return false;
    out = g_aq[g_aq_head];
    g_aq_head = (uint8_t)((g_aq_head + 1) % ANCS_Q_SIZE);
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

static void read_phone_time(bool force = false) {
    if (!g_client) return;
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

    // CTS gives LOCAL time with no timezone. Treat it as the wall clock directly:
    // set TZ=UTC so localtime_r() returns exactly these values (no offset applied).
    // Real UTC + tz only comes from Orion; this backup is display-only — after a
    // power cycle (the only time the iPhone is the sole source) there are no
    // meetings, so the UTC-vs-local epoch distinction doesn't affect any logic.
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(&tmv);
    if (epoch <= 0) return;
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    s_last_cts_ms = millis();
    LOG("[ancs] clock synced from iPhone CTS: %04u-%02u-%02u %02u:%02u\n",
        (unsigned)year, (unsigned)b[2], (unsigned)b[3], (unsigned)b[4], (unsigned)b[5]);
}

// ── ANCS EventFlags bits (Notification Source byte 1) ──────────────────────
namespace EvtFlag {
    constexpr uint8_t SILENT = 0x01, IMPORTANT = 0x02, PREEXISTING = 0x04,
                      POSITIVE_ACTION = 0x08, NEGATIVE_ACTION = 0x10;
}

// ── Pending NS-event metadata ──────────────────────────────────────────────
// EventFlags + CategoryID live in the NS event but aren't repeated in the DS
// attribute response, so stash them by UID until the DS response arrives.
struct PendingMeta { uint32_t uid; uint8_t flags; uint8_t cat; bool used; };
static PendingMeta g_pmeta[8];

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
//   • Known apps  → group by icon token (one icon per app).
//   • Unknown apps (token "unknown") have no brand icon and render a per-category
//     fallback glyph, so grouping them all by token would collapse unrelated
//     apps into a single bell. Instead group unknown notifications by ANCS
//     CATEGORY, so e.g. unknown email apps stack separately from unknown social
//     apps. `cat` is only significant for the "unknown" token.
// Walk newest→oldest so the first occurrence of each group captures the most
// recent UID. Reverse into cfg so cfg[count-1] = newest-active group (rightmost).
static void publish_queue() {
    app_state::AncsConfig cfg = {};
    cfg.phone_connected = true;

    struct Slot { const char* token; uint32_t uid; uint8_t count; uint8_t cat; };
    Slot slots[app_state::MAX_ANCS_NOTIFICATIONS] = {};
    size_t slot_count = 0;

    for (int i = (int)g_queue_count - 1; i >= 0; --i) {
        const char* tok = g_queue[i].icon_token;
        uint32_t    uid = g_queue[i].uid;
        bool    is_unknown = (strcmp(tok, "unknown") == 0);
        uint8_t cat        = is_unknown ? app_state::ancs_category(uid) : 0;

        bool found = false;
        for (size_t j = 0; j < slot_count; ++j) {
            if (strcmp(slots[j].token, tok) == 0 && (!is_unknown || slots[j].cat == cat)) {
                slots[j].count++;
                found = true;
                break;
            }
        }
        if (!found && slot_count < app_state::MAX_ANCS_NOTIFICATIONS) {
            slots[slot_count++] = { tok, uid, 1, cat };
        }
    }

    cfg.count = slot_count;
    for (size_t i = 0; i < slot_count; ++i) {
        size_t src       = slot_count - 1 - i;
        cfg.icons[i]     = slots[src].token;
        cfg.uids[i]      = slots[src].uid;
        cfg.counts[i]    = slots[src].count;
    }

    app_state::set_ancs_config(cfg);
    widget_status_bar::refresh_active();
}

static void queue_add(uint32_t uid, const char* token) {
    // Check for duplicate UID.
    for (size_t i = 0; i < g_queue_count; ++i) {
        if (g_queue[i].uid == uid) return;
    }

    if (g_queue_count >= app_state::MAX_ANCS_NOTIFICATIONS) {
        // Displace oldest (FIFO).
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

    state_machine::set_phone_connected(true);
    LOG("[ancs] queued uid=%u token=%s count=%u\n",
                   (unsigned)uid, token ? token : "?", (unsigned)g_queue_count);
}

static void queue_remove(uint32_t uid) {
    // Drop any stored detail for this notification regardless of whether it's
    // still in the visible queue (it may be a hidden, beyond-the-5th entry).
    app_state::remove_ancs_detail(uid);

    for (size_t i = 0; i < g_queue_count; ++i) {
        if (g_queue[i].uid == uid) {
            memmove(&g_queue[i], &g_queue[i + 1],
                    (g_queue_count - i - 1) * sizeof(ancs_client::QueueEntry));
            g_queue_count--;
            publish_queue();
            LOG("[ancs] removed uid=%u count=%u\n",
                           (unsigned)uid, (unsigned)g_queue_count);
            return;
        }
    }
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

    // Drain NS/DS notifications captured by the host-task callbacks. Runs on the
    // main loop task, so on_notification_source()'s blocking CP write and the
    // queue_add/remove LVGL refresh are both safe here.
    AncsRaw e;
    while (aq_pop(e)) {
        if (e.kind == 0) on_notification_source(e.buf, e.len);
        else             on_data_source(e.buf, e.len);
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
    // banner/dialog and stop the duration timer before clearing state.
    modal_incoming_call::close_all();

    g_conn_handle   = 0xFFFF;
    g_client        = nullptr;
    g_ancs_svc      = nullptr;
    g_ns_char       = nullptr;
    g_cp_char       = nullptr;
    g_ds_char       = nullptr;
    g_phone_name[0] = '\0';
    s_last_cts_ms   = 0;

    // Clear queue and update status bar.
    g_queue_count = 0;
    app_state::AncsConfig cfg = {};
    cfg.phone_connected = false;
    cfg.count = 0;
    app_state::set_ancs_config(cfg);
    widget_status_bar::refresh_active();

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
        // Added or Modified — stash flags+category (not echoed in the DS
        // response) then (re)request attributes. queue_add dedups by UID so a
        // Modified event keeps the existing icon, while set_ancs_detail
        // refreshes the stored title/body/timestamp in place.
        pmeta_put(uid, flags, cat_id);
        ancs_client::request_attributes(uid);
    } else if (event_id == 2) {
        // Removed on the iPhone — close any open overlay / call banner showing
        // it, then drop it from Ori's queue + detail + status bar.
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

    // Requested: AppIdentifier(0), Title(1), Subtitle(2), Message(3), Date(5)
    char app_id[128]   = {};
    char title[193]    = {};
    char subtitle[129] = {};
    char body[513]     = {};
    char date[24]      = {};
    bool got_app_id    = false;

    size_t pos = 5;
    while (pos + 3 <= g_ds_len) {
        uint8_t  attr_id  = g_ds_buf[pos];
        uint16_t attr_len = (uint16_t)(g_ds_buf[pos + 1] | (g_ds_buf[pos + 2] << 8));
        pos += 3;
        if (pos + attr_len > g_ds_len) break; // incomplete — wait for more

        char*  dst = nullptr;
        size_t dst_sz = 0;
        if      (attr_id == 0x00) { dst = app_id;   dst_sz = sizeof(app_id)   - 1; got_app_id = true; }
        else if (attr_id == 0x01) { dst = title;    dst_sz = sizeof(title)    - 1; }
        else if (attr_id == 0x02) { dst = subtitle; dst_sz = sizeof(subtitle) - 1; }
        else if (attr_id == 0x03) { dst = body;     dst_sz = sizeof(body)     - 1; }
        else if (attr_id == 0x05) { dst = date;     dst_sz = sizeof(date)     - 1; }

        if (dst) {
            size_t copy = attr_len < dst_sz ? attr_len : dst_sz;
            memcpy(dst, &g_ds_buf[pos], copy);
            dst[copy] = '\0';
        }
        pos += attr_len;
    }

    if (!got_app_id) return;

    // Recover the NS-event metadata stashed for this UID (flags + category).
    uint8_t flags = 0, cat = app_state::AncsCategory::OTHER;
    pmeta_take(resp_uid, &flags, &cat);
    bool important   = (flags & EvtFlag::IMPORTANT)   != 0;
    bool preexisting = (flags & EvtFlag::PREEXISTING) != 0;

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
    ui::sanitize_text(title,    ftitle, sizeof(ftitle));
    ui::sanitize_text(subtitle, fsub,   sizeof(fsub));
    ui::sanitize_text(body,     fbody,  sizeof(fbody));

    // Store detail BEFORE queue_add so the status-bar tile built during the
    // queue_add → publish_queue → refresh can read this UID's category/importance.
    app_state::set_ancs_detail(resp_uid, token, display_name, ftitle, fsub, fbody,
                               recv_epoch, hhmm, app_id, cat, important);

    // Genuinely-new notifications animate in; the backlog iOS replays on connect
    // (PreExisting flag) populates silently.
    if (!preexisting) widget_status_bar::note_new_notification(resp_uid);

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
    if (is_call && !preexisting && !ota_receiver::is_active()) {
        bool can_answer = (flags & EvtFlag::POSITIVE_ACTION) != 0;
        if (can_answer) modal_incoming_call::show(resp_uid);          // ringing
        else            modal_incoming_call::notify_active(resp_uid); // on call
    }

    queue_add(resp_uid, token);

    // Legacy single-slot pending info (kept for compatibility).
    g_pending_info.uid = resp_uid;
    strncpy(g_pending_info.app_id, app_id, sizeof(g_pending_info.app_id) - 1);
    strncpy(g_pending_info.title,  ftitle, sizeof(g_pending_info.title)  - 1);
    strncpy(g_pending_info.body,   fbody,  sizeof(g_pending_info.body)   - 1);
    g_pending_info.icon_token = token;
    g_pending_valid = true;

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
    //   Message(0x03,512), Date(0x05).
    // Only Title/Subtitle/Message carry a 2-byte max-length; AppIdentifier and
    // Date are fixed-format and take no length field (per the ANCS spec).
    uint8_t cmd[24];
    size_t  n = 0;
    cmd[n++] = 0x00; // GetNotificationAttributes
    cmd[n++] = (uint8_t)(notif_uid & 0xFF);
    cmd[n++] = (uint8_t)((notif_uid >> 8) & 0xFF);
    cmd[n++] = (uint8_t)((notif_uid >> 16) & 0xFF);
    cmd[n++] = (uint8_t)((notif_uid >> 24) & 0xFF);
    cmd[n++] = 0x00;                       // AppIdentifier
    cmd[n++] = 0x01; cmd[n++] = 192 & 0xFF; cmd[n++] = (192 >> 8) & 0xFF;  // Title
    cmd[n++] = 0x02; cmd[n++] = 128 & 0xFF; cmd[n++] = (128 >> 8) & 0xFF;  // Subtitle
    cmd[n++] = 0x03; cmd[n++] = 512 & 0xFF; cmd[n++] = (512 >> 8) & 0xFF;  // Message
    cmd[n++] = 0x05;                       // Date (no length field)

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

const NotificationInfo* pending_notification_info() {
    return g_pending_valid ? &g_pending_info : nullptr;
}

void clear_pending_notification_info() {
    g_pending_valid = false;
}

const QueueEntry* get_queue(size_t* count_out) {
    if (count_out) *count_out = g_queue_count;
    return g_queue;
}

const char* phone_name() {
    return g_phone_name;
}

} // namespace ancs_client

// ── NS / DS notify callbacks (NimBLE task context) ────────────────────────

namespace {

// Host-task context: ONLY copy the bytes into the ring. The actual work
// (blocking CP write, LVGL refresh) is done later by ancs_client::poll() on
// the main task — see the AncsRaw queue comment above.
static void notify_ns_cb(NimBLERemoteCharacteristic* /*c*/,
                          uint8_t* data, size_t len, bool /*is_notify*/) {
    aq_push(0, data, len);
}

static void notify_ds_cb(NimBLERemoteCharacteristic* /*c*/,
                          uint8_t* data, size_t len, bool /*is_notify*/) {
    aq_push(1, data, len);
}

} // namespace
