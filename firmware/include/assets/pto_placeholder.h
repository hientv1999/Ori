#pragma once

#include <stdint.h>
#include <stddef.h>

// Compiled-in PTO placeholder image (528×396 JPEG).
// The .c file is generated from your JPEG via tools/jpg_to_c_array.py and
// placed at firmware/src/assets/pto_placeholder.c.
// Declared extern "C" so the C-generated symbol links from C++ callers.

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char pto_placeholder_jpg[];
extern const unsigned int  pto_placeholder_jpg_len;

#ifdef __cplusplus
}
#endif
