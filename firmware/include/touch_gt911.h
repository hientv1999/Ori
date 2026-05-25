#pragma once
#include <stdint.h>

// GT911 capacitive touch driver (5-point). Coordinates are already in
// panel space (0..799, 0..479) — no transform here.
//
// poll() is non-blocking: returns the current count and fills `out`.
// Returns 0 when nothing is touched. Caller invokes this every loop tick.

struct TouchPoint {
    uint16_t x;
    uint16_t y;
    bool     pressed;
};

namespace touch {

void    init();
uint8_t poll(TouchPoint out[5]);

} // namespace touch
