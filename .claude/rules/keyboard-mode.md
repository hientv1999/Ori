# Ori — Controls Mode

Ori has two top-level modes selected by the user via the **status-bar mode-toggle button**:

- **Calendar mode** — the existing left-panel state machine (meetings / clock / PTO / countdown / reconnect overlay). See `state-machine.md`. The default mode and the only mode available when Orion is offline.
- **Controls mode** — a secondary controller UI for the paired PC: a large album-art panel that doubles as the entire transport + volume interaction surface (tap = play/pause, horizontal swipe = prev/next, vertical swipe = volume with a momentary HUD), currently-playing metadata below it, and three user-assignable shortcut buttons. All commands travel as custom BLE messages to Orion, which bridges them into OS-level actions. See `ble-protocol.md` §12. Internally the firmware/source still refers to this as "keyboard mode" (`kbd-*` class names, `mode: 'keyboard'` enum) for historical reasons; user-facing copy says "Controls".

The status bar and profile card are visible in **both** modes; only the left panel's content changes.

## Mode-toggle button (status bar)

- **Position:** the rightmost element of the status bar at all times *when visible*. Element order in the right cluster, left to right: `[ANCS icons] [phone-disconnect icon (when applicable)] [mode-toggle]`. The toggle never moves — it's the right edge anchor regardless of phone-disconnect state.
- **Hidden when Orion is offline.** When no BLE link to Orion is up, the mode-toggle button is removed from the status bar entirely (no greyed-out state, no inert tap target). Controls mode is useless without Orion bridging commands to the OS, so giving the user a button that does nothing would be misleading. If the user happens to be in Controls mode at the moment Orion drops, the firmware **auto-reverts to calendar mode** before hiding the toggle. When Orion reconnects, the toggle reappears (in calendar mode).
- **Size:** 60 × 60 px, 12 px corner radius. Matches ANCS-icon visual weight without using a brand color.
- **Icon contents:** the icon shows the mode you will **switch to**, not the current mode — so the user reads it as a destination, not a label.
  - In calendar mode → **headphones** glyph on neutral `--screen-elev` background. Means "tap to enter Controls."
  - In Controls mode → **calendar** glyph on accent-tinted `--accent-soft` background, border `--accent-line`, glyph `--accent`. Means "tap to return to calendar." The accent styling makes the active Controls state glanceable from across a desk; the glyph change removes any ambiguity about what a tap will do.
- **Activation:** a single tap. Not a long-press. Long-press is reserved for factory-reset (profile photo) and re-pair-phone (phone-disconnect icon) — see `gestures.md`.
- **Inert during firmware update:** the toggle does not respond while the OTA-Updating screen is showing.

## Left-panel layout (528 × 396)

Three vertical sections, centred horizontally. **No transport buttons and no persistent volume slider** — the album art *is* every transport control AND the volume control. The shortcut row at the bottom is purely the user's assignable slots.

```
┌──────────────── 528 × 396 ────────────────┐
│                                            │
│         ╔════════════════╗                 │  Album art
│         ║                ║                 │  216 × 216 (centred)
│         ║                ║                 │  tap        = play/pause
│         ║    ART         ║                 │  swipe ↔    = prev / next
│         ║                ║                 │  swipe ↕    = volume + HUD
│         ║                ║                 │
│         ╚════════════════╝                 │
│                                            │
│   Industrial Symphony No. 1 — The Dream…   │  Title (centred, 24 px, ellipsis)
│         Angelo Badalamenti                 │  Artist (22 px, secondary, ellipsis)
│                                            │
│    ┌─────┐    ┌─────┐    ┌─────┐          │  Three user-assignable
│    │ 🔇  │    │ 🎙  │    │ 📷  │           │  shortcuts (mock default:
│    └─────┘    └─────┘    └─────┘          │   mute audio / mute mic /
│                                            │   screen capture)
└────────────────────────────────────────────┘
```

### 1. Album art — the interaction surface

216 × 216 px, 14 px corner radius, centred horizontally. Drop shadow for visual lift. Vertical budget: 10 px top margin + 216 px art + ~66 px title/artist block + 90 px shortcut row + 10 px bottom margin = 392 px in 396 px (4 px headroom). Top and bottom margins are equal (10 px each) so the media area feels symmetrically padded. The image is pushed by Orion via the `Media Album Art` characteristic (`ble-protocol.md` §3 char 15) on every track change — Orion reads the OS's now-playing thumbnail, resizes to 180 × 180, JPEG-encodes (~8–15 KB), and chunks it across. Ori decodes JPEG and caches in PSRAM (not NVS — cleared on power cycle, re-fetched on reconnect).

The art carries **three orthogonal gestures**, all on the same surface:

| Gesture | Action | Movement threshold | Visual feedback |
|---|---|---|---|
| **Tap** | Play/pause toggle — emits `KeyboardCommand{op:"play_pause"}` | < 20 px in BOTH axes | Brief centred play- or pause-icon flash (~550 ms, scales up + fades) |
| **Swipe right** | Next track — `{op:"next"}` | > 50 px horizontal AND |dx| > |dy| | Art briefly translates right ~40 px then snaps back |
| **Swipe left** | Previous track — `{op:"prev"}` | mirror of swipe right | Art briefly translates left ~40 px then snaps back |
| **Vertical swipe** | Volume — emits one `KeyboardCommand{op:"vol_set", arg: <0..100>}` on release | > 25 px vertical AND |dy| > |dx| | Volume HUD overlay fades in (vertical bar + percentage), tracks the swipe distance live, lingers ~800 ms after release then fades |

Vertical-swipe sensitivity: **~200 px of swipe = full 0..100 range**. Swipe up = louder, down = quieter. The HUD displays the live mapped value during the gesture so the user can release at the right level. The actual volume change is applied by Orion (via OS volume API) on receipt of the `vol_set` command; Orion then writes the achieved level back to `Host Volume State` (char 13), which Ori reads on next render but ignores while a vertical swipe is in progress (drag-wins local override — see `ble-protocol.md` §12).

**Paused-state treatment:** when the host reports paused, the art dims to ~55 % brightness with a centred play-triangle overlay. Playing state has no overlay and full brightness.

**No-now-playing empty state:** when Orion reports nothing playing, the album-art slot fills with a muted dark radial gradient and the **Ori brand mark** (flat accent-gold outer ring + small solid inner disc — see `memory.md` § Brand Assets) centred, ring diameter 96 px. The title reads "Nothing playing"; the artist is an em-dash. Gestures still fire commands (Orion may interpret a tap to wake/resume).

### 2. Title + artist

Centred horizontally, ~10 px below the art.

- **Title:** 24 px (`font_title`), weight 500, primary text, single-line `text-overflow: ellipsis`. Source: `MediaMetadata.title`.
- **Artist:** 22 px (`font_meta`), secondary text, single-line ellipsis, ~4 px below the title. Source: `MediaMetadata.artist`.

Long titles ellipsise rather than wrap — wrapping would shove the artist row into the shortcut row and break the vertical rhythm.

### 3. Shortcut row

Three icon-only square buttons, 14 px gap, full row width, 90 px tall (8 px top padding + 82 px buttons). Bottom padding below the row is 10 px — matching the 10 px top padding above the art, so the top and bottom margins of the media area are visually equal. Each button fills its share of the row — in the firmware that works out to 152 px wide; in the HTML prototype the buttons use `flex: 1` and span the full available width automatically. Each emits `KeyboardCommand{op:"shortcut", arg: 1|2|3}`. Orion looks up the slot in its local config table and runs the user's configured action — could be a key combo (`SendInput` / `CGEventPost`), an app launch, a script, a macro.

**There is no dedicated persistent mute button on the device.** Mute is just another action the user can assign to a shortcut slot if they want it within reach.

**Default mock config in the prototype** (replaceable by the user in Orion's settings):

| Slot | Mock action | Mock icon |
|---|---|---|
| 1 | Mute audio (toggle OS master mute) | speaker-with-X |
| 2 | Mute mic (toggle system microphone) | mic-with-slash |
| 3 | Screen capture (trigger OS screenshot tool) | screenshot frame |

The icon and host-side action are both configured in Orion's settings UI; on the device the icon is the only label.

## Why the album art carries all four interactions

This UI is a direct outcome of the Orion-as-bridge architecture (`ble-protocol.md` §12). With Orion bridging, any touch on the art becomes a custom command Orion translates to whatever OS action makes sense — no constraint to standard HID codes, no slider needed for absolute volume (Orion knows the level and the gesture maps to it), no separate transport buttons. The art element gets ~35 % more area than the previous design, the volume slider's mispress-risk-against-shortcuts goes away (no slider at all), and the bottom row is purely the user's customisable shortcuts.

If we ever moved to standard BLE HID Over GATT (HOGP), we'd lose the slider+gesture freedom — HOGP can't carry "tap an image to send Play/Pause" or "set OS volume to exactly 47 %." The custom-bridge model is what makes the layout possible.

## What happens on mode entry / exit

- Entering Controls mode does **not** change connectivity, ANCS subscriptions, or sync state. Calendar refreshes still run in the background. The 5-minute pre-meeting countdown timer still fires — and overlays the Controls-mode UI when it does (same as it overlays the meeting list in calendar mode).
- Exiting back to calendar mode restores the priority-ordered state machine in `state-machine.md`.
- The selected mode **persists across power cycles** via NVS, so the user returns to whichever mode they had last selected — *but* if Orion is offline at boot, the device renders calendar mode regardless of the saved preference (and the mode-toggle stays hidden until Orion reconnects).
- **When Orion drops while in Controls mode** (live disconnect): firmware auto-reverts to calendar mode and removes the toggle. This is the only path by which Controls mode is left without a user tap on the toggle.

## What's out of scope here

- BLE characteristic payloads and the Orion-as-bridge command flow → `ble-protocol.md` §3 and §12.
- The Orion-side configuration UI for assigning shortcut actions → `pc-app.md`.
- Touch-event-to-command firmware wiring → M8 implementation work.
- Additional album-art gestures (long-press, pinch, two-finger) → reserved for future use; only tap + swipe-left + swipe-right are wired in v1.
- A persistent host play/pause state characteristic (so Ori knows whether to dim the art independently of the last user action) → may be added in a future minor version if needed.
