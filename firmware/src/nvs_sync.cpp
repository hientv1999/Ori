// Ori NVS Sync Data — M5
//
// SHA-256 hashes and profile strings for the hash-manifest delta reconnect.
// All operations use the same "ori" Preferences namespace as nvs_store.cpp.

#include "nvs_sync.h"

#include <Arduino.h>
#include <Preferences.h>

namespace nvs_sync {

// ── Hash key constants ────────────────────────────────────────────────────────

const char* const HASH_KEY_PROFILE  = "p_sha";
const char* const HASH_KEY_PHOTO    = "ph_sha";
const char* const HASH_KEY_MEETINGS = "m_sha";
const char* const HASH_KEY_PTO      = "pto_sha";

namespace {
constexpr const char* NS = "ori";

// Key names for profile strings and metadata.
constexpr const char* K_NAME   = "p_name";
constexpr const char* K_TITLE  = "p_titl";
constexpr const char* K_EMAIL  = "p_email";
constexpr const char* K_PHONE  = "p_phone";
constexpr const char* K_TZ     = "tz";
constexpr const char* K_EPOCH  = "epoch";

Preferences prefs;

// PTO metadata RAM cache.
//
// ESP-IDF's NVS handle allocator uses SRAM heap. After the media screen
// allocates and frees LVGL objects, the heap layout can place a new
// NVSHandleSimple at an address whose vtable field overlaps a prior JFIF
// buffer, producing a LoadProhibited crash inside load_pto_meta().
//
// Fix: populate once at startup (prime_pto_cache(), clean heap) so that
// all later calls — including from screen_pto::create() — read from RAM.
struct PtoCache {
    uint32_t start;
    uint32_t end;
    char     dest[129];
    bool     loaded;
};
static PtoCache g_pto_cache = {};

static void load_pto_from_nvs() {
    g_pto_cache = {};       // zero fields
    g_pto_cache.loaded = true;  // mark done even if NVS fails
    if (!prefs.begin(NS, /*readOnly=*/true)) return;
    g_pto_cache.start = prefs.getUInt("pto_s", 0);
    g_pto_cache.end   = prefs.getUInt("pto_e", 0);
    String d = prefs.getString("pto_d", "");
    prefs.end();
    size_t dlen = d.length();
    if (dlen >= sizeof(g_pto_cache.dest)) dlen = sizeof(g_pto_cache.dest) - 1;
    memcpy(g_pto_cache.dest, d.c_str(), dlen);
    g_pto_cache.dest[dlen] = '\0';
}
} // namespace

// ── SHA-256 hash storage ───────────────────────────────────────────────────────

bool load_hash(const char* key, uint8_t out_hash[32]) {
    bool ok = false;
    if (prefs.begin(NS, /*readOnly=*/true)) {
        size_t sz = prefs.getBytesLength(key);
        if (sz == 32) {
            prefs.getBytes(key, out_hash, 32);
            ok = true;
        }
        prefs.end();
    }
    return ok;
}

void save_hash(const char* key, const uint8_t hash[32]) {
    if (prefs.begin(NS, /*readOnly=*/false)) {
        prefs.putBytes(key, hash, 32);
        prefs.end();
    }
}

// ── Profile string fields ─────────────────────────────────────────────────────

void save_profile(const char* name, const char* title,
                  const char* email, const char* phone) {
    if (!prefs.begin(NS, /*readOnly=*/false)) return;
    if (name)  prefs.putString(K_NAME,  name);
    if (title) prefs.putString(K_TITLE, title);
    if (email) prefs.putString(K_EMAIL, email);
    if (phone) prefs.putString(K_PHONE, phone);
    prefs.end();
}

bool load_profile(char* out_name,  size_t name_sz,
                  char* out_title, size_t title_sz,
                  char* out_email, size_t email_sz,
                  char* out_phone, size_t phone_sz) {
    if (!prefs.begin(NS, /*readOnly=*/true)) return false;

    String n = prefs.getString(K_NAME,  "");
    String t = prefs.getString(K_TITLE, "");
    String e = prefs.getString(K_EMAIL, "");
    String p = prefs.getString(K_PHONE, "");
    prefs.end();

    if (out_name  && name_sz  > 0) strncpy(out_name,  n.c_str(), name_sz  - 1);
    if (out_title && title_sz > 0) strncpy(out_title, t.c_str(), title_sz - 1);
    if (out_email && email_sz > 0) strncpy(out_email, e.c_str(), email_sz - 1);
    if (out_phone && phone_sz > 0) strncpy(out_phone, p.c_str(), phone_sz - 1);

    return n.length() > 0;
}

// ── Timezone ──────────────────────────────────────────────────────────────────

void save_tz(const char* tz) {
    if (!tz || !tz[0]) return;
    if (prefs.begin(NS, /*readOnly=*/false)) {
        prefs.putString(K_TZ, tz);
        prefs.end();
    }
}

bool load_tz(char* out, size_t sz) {
    if (!prefs.begin(NS, /*readOnly=*/true)) return false;
    String s = prefs.getString(K_TZ, "");
    prefs.end();
    if (s.length() == 0) return false;
    strncpy(out, s.c_str(), sz - 1);
    return true;
}

// ── Last synced epoch ─────────────────────────────────────────────────────────

void save_epoch(uint32_t epoch_utc) {
    if (prefs.begin(NS, /*readOnly=*/false)) {
        prefs.putUInt(K_EPOCH, epoch_utc);
        prefs.end();
    }
}

uint32_t load_epoch() {
    uint32_t v = 0;
    if (prefs.begin(NS, /*readOnly=*/true)) {
        v = prefs.getUInt(K_EPOCH, 0);
        prefs.end();
    }
    return v;
}

// ── PTO metadata ──────────────────────────────────────────────────────────────

void prime_pto_cache() {
    if (!g_pto_cache.loaded) load_pto_from_nvs();
}

void save_pto_meta(uint32_t start, uint32_t end, const char* destination) {
    if (!prefs.begin(NS, /*readOnly=*/false)) return;
    prefs.putUInt("pto_s",  start);
    prefs.putUInt("pto_e",  end);
    if (destination) prefs.putString("pto_d", destination);
    prefs.end();
    // Keep cache in sync so load_pto_meta() doesn't re-read NVS.
    g_pto_cache.start = start;
    g_pto_cache.end   = end;
    if (destination) {
        strncpy(g_pto_cache.dest, destination, sizeof(g_pto_cache.dest) - 1);
        g_pto_cache.dest[sizeof(g_pto_cache.dest) - 1] = '\0';
    } else {
        g_pto_cache.dest[0] = '\0';
    }
    g_pto_cache.loaded = true;
}

bool load_pto_meta(uint32_t* out_start, uint32_t* out_end,
                   char* out_dest, size_t dest_sz) {
    if (!g_pto_cache.loaded) load_pto_from_nvs();
    if (out_start) *out_start = g_pto_cache.start;
    if (out_end)   *out_end   = g_pto_cache.end;
    if (out_dest && dest_sz > 0) {
        strncpy(out_dest, g_pto_cache.dest, dest_sz - 1);
        out_dest[dest_sz - 1] = '\0';
    }
    return g_pto_cache.start != 0;
}

// ── Meeting list CBOR blob ─────────────────────────────────────────────────────

bool save_meetings_blob(const uint8_t* buf, size_t len) {
    if (!buf || !len) return false;
    if (!prefs.begin(NS, /*readOnly=*/false)) return false;
    size_t written = prefs.putBytes("m_cbor", buf, len);
    prefs.end();
    return written == len;
}

size_t load_meetings_blob(uint8_t* out, size_t max_len) {
    if (!out || !max_len) return 0;
    if (!prefs.begin(NS, /*readOnly=*/true)) return 0;
    size_t sz = prefs.getBytesLength("m_cbor");
    if (!sz || sz > max_len) { prefs.end(); return 0; }
    prefs.getBytes("m_cbor", out, sz);
    prefs.end();
    return sz;
}

} // namespace nvs_sync
