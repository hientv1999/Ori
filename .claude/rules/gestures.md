# Ori — Gestures and Interactions

## Gesture Reference

| Gesture | Action | Available In |
|---|---|---|
| Two-finger swipe up | Turn screen backlight **ON** | Every state |
| Two-finger swipe down | Turn screen backlight **OFF** | Every state |
| Long-press profile photo (3 s) | Open factory reset confirmation | Every state |
| Long-press phone-disconnect icon (3 s) | Open re-pair iPhone screen | Every runtime state (not during first-boot setup) |
| **Tap mode-toggle button** (rightmost status-bar element) | Switch between calendar mode and **Controls** mode | Every runtime state — except: not during setup, not during firmware update, and **not when Orion is offline** (the toggle is hidden entirely in the offline case — see `keyboard-mode.md`) |
| Tap Close button on countdown modal | Dismiss countdown | Countdown modal only |
| **Tap album art** (movement < 20 px in both axes) | Emit `KeyboardCommand{op:"play_pause"}` to Orion | Controls mode only — see `keyboard-mode.md` |
| **Swipe right on album art** (horizontal > 50 px, |dx|>|dy|) | Emit `KeyboardCommand{op:"next"}` | Controls mode only |
| **Swipe left on album art** | Emit `KeyboardCommand{op:"prev"}` | Controls mode only |
| **Vertical swipe on album art** (|dy|>25 px, |dy|>|dx|; ~200 px = full 0..100 range) | Emit `KeyboardCommand{op:"vol_set", arg:N}` on release. Momentary HUD overlay shows the live mapped level during the swipe. | Controls mode only |
| Tap shortcut button (1/2/3) | Emit `KeyboardCommand{op:"shortcut", arg:1|2|3}` — Orion runs the user's configured action | Controls mode only |

## Backlight Control

The backlight is **binary**: ON or OFF. There is no dimming. This is a hardware constraint of the Waveshare ESP32-S3-Touch-LCD-4.3 board — the backlight enable line lives on a digital-only CH422G I/O expander (EXIO2) with no PWM-capable path from the ESP32. See `memory.md` for the full constraint.

Turning the backlight OFF does **not** put the device to sleep. The ESP32-S3 stays fully running — BLE links stay up, calendar refreshes, the meeting list still tracks expiry, the 5-minute countdown timer still fires. Only the visible LED is gated off. Swiping up brings the screen back instantly with the current state already drawn.

Backlight state is **bidirectional**: it can be toggled either by the two-finger swipe gesture on Ori, or by Orion's ON/OFF toggle button in the PC app's settings. Both paths update the same NVS-persisted value and notify the other side over BLE so the toggle stays in sync. See `ble-protocol.md` Backlight characteristic (`0009`).

### Gesture (local input)

- **Recognition**: exactly two touch points, both moving predominantly vertically (vertical travel > horizontal travel).
- **Engagement**: fingers must be present for ≥ 80 ms and travel ≥ 60 px vertically from the initial two-finger touchdown. The longer travel threshold (vs. M2's 10 px) makes the gesture deliberate — accidental two-finger taps will not toggle the backlight.
- **One-shot**: once a swipe fires (ON or OFF), it does not fire again until both fingers lift and a new touchdown begins. The user cannot rapidly toggle by oscillating their fingers.
- **Direction**: a net upward swipe (`dy < 0`) turns the backlight ON; a net downward swipe (`dy > 0`) turns it OFF. Direction is measured against the initial touchdown position, not instantaneous motion.
- **Single-touch suspended** while two fingers are active; resumed when fewer than two fingers remain.
- **Idempotent**: swiping up while the backlight is already on is a no-op. Same for swiping down while it is already off.
- After the gesture fires, Ori notifies the new state over the Backlight characteristic so Orion's toggle stays in sync.

### Orion toggle (remote input)

- Orion exposes an explicit **ON / OFF toggle button** in its settings (not a slider). The label reads "Screen backlight".
- On tap, Orion writes a single byte to the Backlight characteristic — `0x00` for OFF, `0x01` for ON.
- Ori applies it immediately via CH422G EXIO2 and queues an NVS write (same debounce as gesture).
- No notify is needed in this direction — Orion already knows the value it just sent.

### Shared rules

- **Persistence**: saved to NVS with ~2 s debounce after the last change (from either source). Restored before the panel enables on boot — the backlight comes up at the saved state, never with a momentary flash.
- **Factory reset**: clears saved backlight state; device boots with backlight **ON**.
- **Inert during firmware update**: the gesture is disabled and remote BLE writes are NACKed while a firmware update is in progress over USB CDC (see `ota.md`). The update progress screen is always visible (forces backlight ON for the duration of the update).
