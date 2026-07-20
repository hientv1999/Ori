# Ori — Gestures and Interactions

## Gesture Reference

| Gesture | Action | Available In |
|---|---|---|
| **Tap status-bar date/time** | Enter Clock view — tap mode-toggle button to return | Every runtime state (status bar hidden during setup + OTA) |
| **Long-press status-bar date/time (1 s)** | Enter Calendar (month view) — tap mode-toggle button to return | Every runtime state (status bar hidden during setup + OTA) |
| Tap a holiday day cell in Calendar (month view) | Opens a full-screen detail overlay with the holiday's name, full date, and a short description — Close button only | Calendar (month view) only — non-holiday cells are inert |
| Long-press profile photo (3 s) | Open factory reset confirmation | Every state |
| **Tap phone icon** (always visible; always neutral colour — disconnected adds a diagonal slash, not a colour change) | Bonded (connected or not) → iPhone Info/Stats modal; not bonded → re-pair iPhone screen (stale bond auto-wiped) | Every runtime state (not during setup) |
| Tap a missed-call/message/notification count tile in the iPhone Info modal | Opens that category's drill-down list (grouped by sender, newest first) — inert when disconnected or the count is zero | iPhone Info modal only |
| Tap a row in the ANCS drill-down list | Opens the row's full detail overlay (same one status-bar icons use) — "Read all" for a stacked group | ANCS drill-down list only |
| **Swipe left a row in the ANCS drill-down list** | Clears that notification (or its whole stacked group) — row slides and fades with the drag, commits past ~35% of its width | ANCS drill-down list only |
| Tap an ANCS status-bar icon | Opens that notification's detail overlay (title + body, Close only) — or, for a still-ringing/active call, the Answer/Decline/End call dialog instead | Every runtime state |
| **Tap mode-toggle button** (rightmost status-bar element) | Cycle calendar ↔ Controls mode | Runtime only — hidden until Orion has fully synced (not merely connected), during setup, and during OTA |
| Tap Close button on countdown modal | Dismiss countdown | Countdown modal only |
| **Tap album art** (movement < 20 px in both axes, not on the button/progress bar) | Toggles the touch-revealed controls (scrim + progress bar + circular play/pause button): reveals if hidden, dismisses if shown — resolved only after `DOUBLE_TAP_MS` (400 ms) with no follow-up tap in the same half, so it isn't mistaken for half of a double-tap | Controls mode — see `media-mode.md` |
| **Tap the circular play/pause button** (centred on the art) | `KeyboardCommand{op:"play_pause"}` — the only way to toggle playback locally | Controls mode — see `media-mode.md` |
| **Double-tap left/right half of album art** (two taps in the same half within 400 ms) | `KeyboardCommand{op:"seek", arg:position_s}` — jumps backward (left half) or forward (right half) by the configured seek step (default 10 s, Orion-configurable); never changes the button/progress bar's visibility | Controls mode — see `media-mode.md` |
| **Swipe right on album art** (|dx| > 50 px, |dx| > |dy|) | `KeyboardCommand{op:"next"}` — never changes the button/progress bar's visibility | Controls mode |
| **Swipe left on album art** | `KeyboardCommand{op:"prev"}` — same non-interference | Controls mode |
| **Vertical swipe on album art** (|dy| > 25 px, |dy| > |dx|; ~400 px = full 0..100) | `KeyboardCommand{op:"vol_set", arg:N}` on release + momentary volume HUD — only the (separate) volume HUD is affected, never the button/progress bar | Controls mode |
| Tap shortcut button (1/2/3) | `KeyboardCommand{op:"shortcut", arg:1|2|3}` — Orion runs configured action | Controls mode |
