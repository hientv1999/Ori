#include "gesture.h"

#include <Arduino.h>
#include <stdlib.h>

#include "backlight.h"

// Two-finger discrete swipe → backlight ON/OFF.
//
// State machine:
//   Idle           — fewer than 2 fingers down (or just lifted)
//   PendingEngage  — 2 fingers detected, waiting for 80 ms presence + 60 px Y
//   Fired          — gesture fired this contact; absorb further motion as
//                    a no-op until both fingers lift (prevents oscillation)

namespace {

// memory.md fixed constants.
constexpr uint32_t ENGAGE_TIME_MS    = 80;
constexpr int32_t  ENGAGE_DELTA_PX   = 60;       // deliberate swipe, not tap
constexpr uint8_t  REQUIRED_FINGERS  = 2;

enum class Phase : uint8_t {
    Idle,
    PendingEngage,
    Fired,
};

Phase    phase = Phase::Idle;
uint32_t touchdown_ms = 0;
int32_t  touchdown_avg_y = 0;

void reset_state() {
    phase = Phase::Idle;
    touchdown_ms = 0;
    touchdown_avg_y = 0;
}

bool two_fingers_pressed(const TouchPoint pts[5], uint8_t count, int32_t& avg_y) {
    if (count != REQUIRED_FINGERS) return false;
    if (!pts[0].pressed || !pts[1].pressed) return false;
    avg_y = ((int32_t)pts[0].y + (int32_t)pts[1].y) / 2;
    return true;
}

} // namespace

namespace gesture {

void update(const TouchPoint points[5], uint8_t count) {
    int32_t avg_y = 0;
    bool two = two_fingers_pressed(points, count, avg_y);

    if (!two) {
        // Either fewer than 2 fingers, or count != 2 (e.g. 3+). Reset so the
        // next clean 2-finger contact can engage again.
        if (phase != Phase::Idle) reset_state();
        return;
    }

    if (phase == Phase::Fired) {
        // Already fired on this contact — absorb further motion. Waiting for
        // a lift to reset.
        return;
    }

    if (phase == Phase::Idle) {
        phase = Phase::PendingEngage;
        touchdown_ms    = millis();
        touchdown_avg_y = avg_y;
        return;
    }

    // phase == PendingEngage
    uint32_t elapsed = millis() - touchdown_ms;
    if (elapsed < ENGAGE_TIME_MS) return;

    int32_t dy_total = avg_y - touchdown_avg_y;  // + = downward, - = upward
    if (abs(dy_total) < ENGAGE_DELTA_PX) return;

    // Threshold crossed in a clear direction → fire one-shot.
    if (dy_total < 0) {
        // Upward swipe → backlight ON. set_on() is idempotent.
        backlight::set_on(true);
        Serial.printf("[gesture] swipe up  dy=%ld -> backlight ON\n", (long)dy_total);
    } else {
        // Downward swipe → backlight OFF.
        backlight::set_on(false);
        Serial.printf("[gesture] swipe dn  dy=%ld -> backlight OFF\n", (long)dy_total);
    }

    phase = Phase::Fired;
}

} // namespace gesture
