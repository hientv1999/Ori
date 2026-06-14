#include "arduino_shim.h"
#include "state_machine.h"

#include <chrono>
#include <lvgl.h>

extern "C" uint32_t millis(void) {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    auto delta = clock::now() - start;
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
}

// ---------------------------------------------------------------------------
// Stubs for firmware modules not linked into the simulator
// ---------------------------------------------------------------------------

namespace state_machine {
    AppState evaluate() { return AppState::MEETING_LIST; }
    void on_factory_reset() {}
    void hold_for_setup_complete() {}
    void on_setup_complete() {}
    void on_mode_toggle() {}
    void on_ota_begin() {}
    void on_reconnect_begin() {}
    void on_reconnect_end() {}
    void on_unpair_phone() {}
}

// ui::make_screen_body and ui::make_panel_divider are implemented in
// firmware/src/ui_helpers.cpp which is compiled directly into the sim.
