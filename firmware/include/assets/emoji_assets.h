#pragma once
#include <lvgl.h>
#include <stdint.h>

// Codepoint -> compiled-in Fluent emoji image (sorted by cp).
typedef struct { uint32_t cp; const lv_image_dsc_t* img; } ori_emoji_entry_t;
extern const ori_emoji_entry_t g_ori_emoji[];
extern const uint32_t g_ori_emoji_count;
