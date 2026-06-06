// Ori — profile photo + PTO destination image decode-once PSRAM cache.
//
// Both images are 228×228 JPEG (≤40 KB profile, ≤64 KB PTO).
// Raw JPEG persisted to NVS; decoded RGB565 held in PSRAM.
// LVGL renders directly from PSRAM — zero re-decode per frame.
//
// Empty image (len == 0 in store_pto) means the user set no destination
// photo in Orion; get_pto() returns nullptr and screen_pto uses the gradient.

#include "photo_cache.h"

#include <Arduino.h>
#include "ori_log.h"
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <time.h>

#include "libs/tjpgd/tjpgd.h"

#include "widgets/widget_profile_card.h"

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint16_t PHOTO_W     = 228;
static constexpr uint16_t PHOTO_H     = 228;
static constexpr size_t   RGB565_SIZE = PHOTO_W * PHOTO_H * 2; // 104,832 bytes

static constexpr size_t PROFILE_MAX_JPEG = 40 * 1024;
static constexpr size_t PTO_MAX_JPEG     = 64 * 1024;

// ── Shared tjpgd decode plumbing ──────────────────────────────────────────────

namespace {

// Context set before each jd_decomp call; the write callback reads from here.
struct DecodeCtx {
    const uint8_t* jpeg;
    size_t         pos;
    size_t         len;
    uint16_t*      out_buf;
    uint16_t       out_w;
    uint16_t       out_h;
};

static DecodeCtx g_dec;

static size_t jpeg_read_cb(JDEC* jd, uint8_t* buf, size_t nb) {
    (void)jd;
    size_t avail = g_dec.len - g_dec.pos;
    if (nb > avail) nb = avail;
    if (buf) memcpy(buf, g_dec.jpeg + g_dec.pos, nb);
    g_dec.pos += nb;
    return nb;
}

// tjpgd emits RGB888 (JD_FORMAT=0 in tjpgdcnf.h: 3 bytes/pixel R,G,B).
// Convert on the fly to RGB565 and write into g_dec.out_buf.
static int jpeg_write_cb(JDEC* jd, void* bitmap, JRECT* rect) {
    (void)jd;
    if (!g_dec.out_buf) return 0;

    const uint8_t* src = static_cast<const uint8_t*>(bitmap);
    uint16_t bw = (uint16_t)(rect->right  - rect->left + 1);
    uint16_t bh = (uint16_t)(rect->bottom - rect->top  + 1);

    for (uint16_t row = 0; row < bh; ++row) {
        uint16_t y = rect->top + row;
        if (y >= g_dec.out_h) break;
        for (uint16_t col = 0; col < bw; ++col) {
            uint16_t x = rect->left + col;
            if (x >= g_dec.out_w) { src += 3; continue; }
            uint8_t r = *src++;
            uint8_t g = *src++;
            uint8_t b = *src++;
            uint16_t px = (uint16_t)(((r & 0xF8u) << 8) |
                                     ((g & 0xFCu) << 3) |
                                     ( b          >> 3));
            g_dec.out_buf[y * g_dec.out_w + x] = px;
        }
    }
    return 1;
}

// Decode jpeg into out_buf (pre-allocated, PHOTO_W × PHOTO_H × 2 bytes).
static bool decode_jpeg_to(const uint8_t* jpeg, size_t len,
                            uint16_t* out_buf, uint16_t w, uint16_t h) {
    static uint8_t work[4096];
    g_dec = { jpeg, 0, len, out_buf, w, h };

    JDEC jd = {};
    JRESULT res = jd_prepare(&jd, jpeg_read_cb, work, sizeof(work), nullptr);
    if (res != JDR_OK) {
        LOG("[photo_cache] jd_prepare failed: %d\n", (int)res);
        return false;
    }
    if (jd.width > w || jd.height > h) {
        LOG("[photo_cache] JPEG too large: %ux%u (max %ux%u)\n",
                      (unsigned)jd.width, (unsigned)jd.height,
                      (unsigned)w, (unsigned)h);
        return false;
    }
    res = jd_decomp(&jd, jpeg_write_cb, 0);
    if (res != JDR_OK) {
        LOG("[photo_cache] jd_decomp failed: %d\n", (int)res);
        return false;
    }
    return true;
}

// Build an lv_image_dsc_t pointing at a PSRAM pixel buffer.
static void build_dsc(lv_image_dsc_t* dsc, const uint16_t* buf,
                      uint16_t w, uint16_t h) {
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags  = 0;
    dsc->header.w      = w;
    dsc->header.h      = h;
    dsc->header.stride = (uint32_t)w * 2;
    dsc->data_size     = (uint32_t)w * h * 2;
    dsc->data          = reinterpret_cast<const uint8_t*>(buf);
}

// Load a JPEG blob from NVS, decode to a PSRAM buffer.
// Returns the PSRAM buffer (caller must build_dsc + set ready), or nullptr on failure.
// NVS namespace: ns, keys "jpg" (blob) and "jpg_sz" (uint32).
static uint16_t* load_and_decode(const char* ns, size_t max_jpeg_bytes,
                                  uint16_t w, uint16_t h) {
    Preferences prefs;
    if (!prefs.begin(ns, /*readOnly=*/false)) return nullptr;
    uint32_t sz = prefs.getUInt("jpg_sz", 0);
    if (sz == 0 || sz > max_jpeg_bytes) {
        prefs.end();
        return nullptr;
    }

    uint8_t* jpeg_buf = static_cast<uint8_t*>(
        heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jpeg_buf) jpeg_buf = static_cast<uint8_t*>(malloc(sz));
    if (!jpeg_buf) { prefs.end(); return nullptr; }

    size_t got = prefs.getBytes("jpg", jpeg_buf, sz);
    prefs.end();

    if (got != sz) {
        LOG("[photo_cache] NVS read mismatch (got=%u want=%u)\n",
                      (unsigned)got, (unsigned)sz);
        free(jpeg_buf);
        return nullptr;
    }

    uint16_t* rgb_buf = static_cast<uint16_t*>(
        heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM));
    if (!rgb_buf) {
        LOG("[photo_cache] PSRAM alloc failed\n");
        free(jpeg_buf);
        return nullptr;
    }

    bool ok = decode_jpeg_to(jpeg_buf, sz, rgb_buf, w, h);
    free(jpeg_buf);

    if (!ok) {
        heap_caps_free(rgb_buf);
        return nullptr;
    }
    return rgb_buf;
}

// Persist a JPEG blob to NVS and decode to a PSRAM buffer.
static uint16_t* save_and_decode(const char* ns, size_t max_jpeg_bytes,
                                   const uint8_t* jpeg, size_t len,
                                   uint16_t w, uint16_t h) {
    {
        Preferences prefs;
        if (prefs.begin(ns, /*readOnly=*/false)) {
            prefs.putUInt("jpg_sz", (uint32_t)len);
            prefs.putBytes("jpg", jpeg, len);
            prefs.end();
        }
    }

    uint16_t* rgb_buf = static_cast<uint16_t*>(
        heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM));
    if (!rgb_buf) {
        LOG("[photo_cache] PSRAM alloc failed\n");
        return nullptr;
    }
    if (!decode_jpeg_to(jpeg, len, rgb_buf, w, h)) {
        heap_caps_free(rgb_buf);
        return nullptr;
    }
    return rgb_buf;
}

static void erase_nvs(const char* ns) {
    Preferences prefs;
    if (prefs.begin(ns, /*readOnly=*/false)) {
        prefs.remove("jpg");
        prefs.remove("jpg_sz");
        prefs.end();
    }
}

// ── Profile photo state ───────────────────────────────────────────────────────

uint16_t*      g_profile_buf  = nullptr;
lv_image_dsc_t g_profile_dsc  = {};
bool           g_profile_ready = false;

// ── PTO image state ───────────────────────────────────────────────────────────

uint16_t*      g_pto_buf   = nullptr;
lv_image_dsc_t g_pto_dsc   = {};
bool           g_pto_ready  = false;

} // namespace

// ── Profile photo public API ──────────────────────────────────────────────────

namespace photo_cache {

void init() {
    uint16_t* buf = load_and_decode("photo", PROFILE_MAX_JPEG, PHOTO_W, PHOTO_H);
    if (buf) {
        if (g_profile_buf) heap_caps_free(g_profile_buf);
        g_profile_buf   = buf;
        g_profile_ready = true;
        build_dsc(&g_profile_dsc, g_profile_buf, PHOTO_W, PHOTO_H);
        // Register with the widget so any already-created profile card shows
        // the photo immediately, and future cards start with it loaded.
        widget_profile_card::set_photo(&g_profile_dsc);
        LOG("[photo_cache] profile photo loaded from NVS\n");
    }
    // No photo in NVS → ensure the widget shows the initials placeholder.
    // (Covers the case where a previous call left g_default_photo set.)
    else {
        widget_profile_card::set_photo(nullptr);
    }
}

void store(uint8_t* jpeg, size_t len) {
    if (len == 0) {
        // Orion sent an empty photo payload — user cleared their profile photo.
        // Erase NVS + PSRAM and revert the widget to the initials placeholder.
        if (jpeg) heap_caps_free(jpeg);
        LOG("[photo_cache] store: empty — clearing profile photo\n");
        clear();
        return;
    }
    if (!jpeg || len > PROFILE_MAX_JPEG) {
        if (jpeg) heap_caps_free(jpeg);
        LOG("[photo_cache] store: invalid args (len=%u)\n", (unsigned)len);
        return;
    }

    uint16_t* buf = save_and_decode("photo", PROFILE_MAX_JPEG, jpeg, len,
                                     PHOTO_W, PHOTO_H);
    heap_caps_free(jpeg);

    if (buf) {
        if (g_profile_buf) heap_caps_free(g_profile_buf);
        g_profile_buf   = buf;
        g_profile_ready = true;
        build_dsc(&g_profile_dsc, g_profile_buf, PHOTO_W, PHOTO_H);
        LOG("[photo_cache] profile photo stored (%u bytes)\n", (unsigned)len);
        widget_profile_card::set_photo(&g_profile_dsc);
    } else {
        LOG("[photo_cache] profile photo decode failed\n");
    }
}

const lv_image_dsc_t* get() {
    return g_profile_ready ? &g_profile_dsc : nullptr;
}

void clear() {
    g_profile_ready = false;
    erase_nvs("photo");
    if (g_profile_buf) { heap_caps_free(g_profile_buf); g_profile_buf = nullptr; }
    g_profile_dsc = {};
    // Revert any live profile card to the initials placeholder.
    widget_profile_card::set_photo(nullptr);
    LOG("[photo_cache] profile photo cleared\n");
}

// ── PTO destination image public API ─────────────────────────────────────────

void init_pto() {
    uint16_t* buf = load_and_decode("pto_img", PTO_MAX_JPEG, PHOTO_W, PHOTO_H);
    if (buf) {
        if (g_pto_buf) heap_caps_free(g_pto_buf);
        g_pto_buf   = buf;
        g_pto_ready = true;
        build_dsc(&g_pto_dsc, g_pto_buf, PHOTO_W, PHOTO_H);
        LOG("[photo_cache] PTO image loaded from NVS\n");
    }
}

void store_pto(uint8_t* jpeg, size_t len) {
    // len == 0 means Orion sent no image (user didn't set one) — clear cache.
    if (len == 0) {
        if (jpeg) heap_caps_free(jpeg);
        clear_pto();
        return;
    }
    if (len > PTO_MAX_JPEG) {
        LOG("[photo_cache] store_pto: JPEG too large (%u bytes)\n", (unsigned)len);
        if (jpeg) heap_caps_free(jpeg);
        return;
    }

    uint16_t* buf = save_and_decode("pto_img", PTO_MAX_JPEG, jpeg, len,
                                     PHOTO_W, PHOTO_H);
    heap_caps_free(jpeg);

    if (buf) {
        if (g_pto_buf) heap_caps_free(g_pto_buf);
        g_pto_buf   = buf;
        g_pto_ready = true;
        build_dsc(&g_pto_dsc, g_pto_buf, PHOTO_W, PHOTO_H);
        LOG("[photo_cache] PTO image stored (%u bytes)\n", (unsigned)len);
    } else {
        LOG("[photo_cache] PTO image decode failed\n");
    }
}

const lv_image_dsc_t* get_pto() {
    return g_pto_ready ? &g_pto_dsc : nullptr;
}

void clear_pto() {
    g_pto_ready = false;
    erase_nvs("pto_img");
    if (g_pto_buf) { heap_caps_free(g_pto_buf); g_pto_buf = nullptr; }
    g_pto_dsc = {};
    LOG("[photo_cache] PTO image cleared\n");
}

} // namespace photo_cache
