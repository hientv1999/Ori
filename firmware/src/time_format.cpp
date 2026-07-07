#include "time_format.h"

#include <stdio.h>

#include "nvs_store.h"

namespace time_format {
namespace {
// RAM cache of the NVS-persisted setting: 0 = 24-hour, 1 = 12-hour.
uint8_t g_fmt = 0;
} // namespace

void init() {
    g_fmt = nvs::get_time_format();
}

uint8_t get() {
    return g_fmt;
}

bool is_24h() {
    return g_fmt == 0;
}

void set(uint8_t fmt) {
    g_fmt = fmt ? 1 : 0;
    nvs::set_time_format(g_fmt);
}

void hhmm(char* out, size_t sz, int hour24, int min) {
    if (!out || sz == 0) return;
    if (g_fmt == 0) {
        snprintf(out, sz, "%02d:%02d", hour24, min);
    } else {
        int h12 = hour24 % 12;
        if (h12 == 0) h12 = 12;
        snprintf(out, sz, "%d:%02d %s", h12, min, hour24 < 12 ? "AM" : "PM");
    }
}

void hhmm_split(char* out_time, size_t time_sz, char* out_suffix, size_t suffix_sz,
                int hour24, int min) {
    if (out_time && time_sz) out_time[0] = '\0';
    if (out_suffix && suffix_sz) out_suffix[0] = '\0';
    if (!out_time || time_sz == 0) return;
    if (g_fmt == 0) {
        snprintf(out_time, time_sz, "%02d:%02d", hour24, min);
    } else {
        int h12 = hour24 % 12;
        if (h12 == 0) h12 = 12;
        snprintf(out_time, time_sz, "%d:%02d", h12, min);
        if (out_suffix && suffix_sz) snprintf(out_suffix, suffix_sz, "%s", hour24 < 12 ? "AM" : "PM");
    }
}

void reformat(const char* hhmm24, char* out, size_t sz) {
    if (!out || sz == 0) return;
    out[0] = '\0';
    if (!hhmm24 || !hhmm24[0]) return;
    int h = 0, m = 0;
    const char* p = hhmm24;
    while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
    if (*p == ':') ++p;
    while (*p >= '0' && *p <= '9') m = m * 10 + (*p++ - '0');
    hhmm(out, sz, h, m);
}

} // namespace time_format
