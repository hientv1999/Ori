# Ori — Gestures and Interactions

## Gesture Reference

| Gesture | Action | Available In |
|---|---|---|
| **Tap status-bar date/time** | Enter Clock view — tap mode-toggle button to return | Every runtime state (status bar hidden during setup + OTA) |
| **Long-press status-bar date/time (1 s)** | Enter Calendar (month view) — tap mode-toggle button to return | Every runtime state (status bar hidden during setup + OTA) |
| Long-press profile photo (3 s) | Open factory reset confirmation | Every state |
| **Tap phone icon** (always visible; neutral = connected, red = disconnected) | Connected → Unpair iPhone modal; disconnected → re-pair iPhone screen (stale bond auto-wiped) | Every runtime state (not during setup) |
| **Tap mode-toggle button** (rightmost status-bar element) | Cycle calendar ↔ Controls mode | Runtime only — hidden when Orion offline, during setup, and during OTA |
| Tap Close button on countdown modal | Dismiss countdown | Countdown modal only |
| **Tap album art** (movement < 20 px in both axes) | `KeyboardCommand{op:"play_pause"}` | Controls mode — see `media-mode.md` |
| **Swipe right on album art** (|dx| > 50 px, |dx| > |dy|) | `KeyboardCommand{op:"next"}` | Controls mode |
| **Swipe left on album art** | `KeyboardCommand{op:"prev"}` | Controls mode |
| **Vertical swipe on album art** (|dy| > 25 px, |dy| > |dx|; ~200 px = full 0..100) | `KeyboardCommand{op:"vol_set", arg:N}` on release + momentary volume HUD | Controls mode |
| Tap shortcut button (1/2/3) | `KeyboardCommand{op:"shortcut", arg:1|2|3}` — Orion runs configured action | Controls mode |
