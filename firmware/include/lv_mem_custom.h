#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* lv_psram_malloc(size_t size);
void* lv_psram_realloc(void* ptr, size_t size);
void  lv_psram_free(void* ptr);

#ifdef __cplusplus
}
#endif
