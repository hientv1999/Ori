# Ori — Media mode

Ori has two user-selectable modes via the **status-bar mode-toggle button**:

- **Calendar mode** (default) — meeting list / "No meetings today." Always available. See `state-machine.md`.
- **Media mode** — album-art transport + volume UI for the paired PC. Commands travel as BLE to Orion, which bridges to OS APIs. **Only available when Orion is connected** — auto-reverts to Calendar on PC disconnect. Internal code name: `kbd-mode` (historical; user-facing copy says "Controls").

**Clock** is a third view entered by tapping the status-bar date/time — not part of the toggle cycle. The mode-toggle becomes a return button while in Clock. See `state-machine.md`.

Status bar and profile card are visible in all three modes.

## Mode-toggle button (status bar)

- **Position:** rightmost status-bar element always. Right-cluster order: `[ANCS icons] [phone-disconnect (when applicable)] [mode-toggle]`.
- **Hidden when Orion is offline** — except in Clock, where it always shows as a return button. On PC disconnect while in Media mode: auto-revert to Calendar + hide toggle.
- **Size:** 60 × 60 px, 12 px corner radius.
- **Icon shows destination, not current state:**
  - Calendar mode → headphones on neutral `--screen-elev` background ("tap to enter Controls")
  - Media mode → calendar on `--accent-soft` background + `--accent-line` border, glyph `--accent` ("tap to return to Calendar")
  - Clock view → calendar on neutral `--screen-elev` background ("tap to return to previous mode")
- **Activation:** single tap. Inert during OTA update.

## Left-panel layout (528 × 396)

Three vertical sections, centred. No transport buttons; no persistent volume slider — the album art is every control.

```
+---------------- 528 x 396 ----------------+
|                                            |
|         +----------------+                |  Album art 216x216 px
|         |                |                |  tap     = play/pause
|         |    ART         |                |  swipe h = prev / next
|         |                |                |  swipe v = volume + HUD
|         +----------------+                |
|                                            |
|   Industrial Symphony No. 1 - The Dream…  |  Title (24 px, ellipsis)
|         Angelo Badalamenti                 |  Artist (22 px, secondary)
|                                            |
|    +-----+    +-----+    +-----+          |  3 user-assignable
|    |  1  |    |  2  |    |  3  |          |  shortcuts
|    +-----+    +-----+    +-----+          |
+--------------------------------------------+
```

### 1. Album art — the interaction surface

216 × 216 px, 14 px corner radius, centred, drop shadow. Vertical budget: 10 + 216 + ~66 + 90 + 10 = 392 px (4 px headroom in 396 px). Pushed by Orion via `Media Album Art` char on every track change — resized to 180 × 180, JPEG (~8–15 KB), chunked. Ori decodes and PSRAM-caches (not NVS; re-fetched on reconnect).

| Gesture | Action | Threshold | Feedback |
|---|---|---|---|
| **Tap** | `{op:"play_pause"}` | < 20 px in both axes | Play/pause icon flash (~550 ms, scales + fades) |
| **Swipe right** | `{op:"next"}` | > 50 px horizontal, \|dx\| > \|dy\| | Art shifts right ~40 px then snaps back |
| **Swipe left** | `{op:"prev"}` | mirror of swipe right | Art shifts left ~40 px then snaps back |
| **Vertical swipe** | `{op:"vol_set", arg:N}` on release | > 25 px vertical, \|dy\| > \|dx\| | Volume HUD (bar + %) fades in, tracks live, lingers ~800 ms |

Volume sensitivity: ~200 px swipe = full 0..100 range. Swipe up = louder. Orion applies volume via OS API and writes back `Host Volume State`; Ori ignores incoming pushes while a swipe is in progress (drag-wins override — see `ble-protocol.md` §12).

**Paused:** art dims to ~55% with centred play-triangle overlay.

**Nothing playing:** slot shows a muted dark radial gradient with the **Ori wordmark** centred. Title = "Nothing playing"; artist = em-dash. Gestures still emit commands (Orion may interpret a tap to wake/resume).

### 2. Title + artist

Centred, ~10 px below art. Single-line ellipsis only — no wrapping.

- **Title:** 24 px (`font_title`), weight 500, primary text. Source: `MediaMetadata.title`.
- **Artist:** 22 px (`font_meta`), secondary text, 4 px below title. Source: `MediaMetadata.artist`.

### 3. Shortcut row

Three icon-only square buttons, 82 px tall, 14 px gap, full row width (~152 px wide each in firmware). 8 px top padding, 10 px bottom padding. Each emits `KeyboardCommand{op:"shortcut", arg:1|2|3}`; Orion runs the configured action.

**Icon assets are compiled into firmware flash.** Ori ships with a fixed set of icon glyphs. Orion's settings UI lets the user pick one per slot from that set and communicates the selection to Ori as an icon ID. Adding new icon types to the available set requires a firmware update — there is no runtime asset delivery path.

Supported icon tokens (5 total):

| Token | Action |
|---|---|
| `vol-mute` | Toggle OS master mute |
| `mic-mute` | Toggle system microphone |
| `screenshot` | Screenshot |
| `lock-screen` | Lock screen |
| `favorite` | User-configured custom action (set in Orion settings) |

Default mock config: slot 1 = `vol-mute`, slot 2 = `mic-mute`, slot 3 = `screenshot`.

## Mode entry / exit

- Entering Media mode does not affect connectivity, ANCS, or sync. Calendar refreshes run in the background. The 5-minute countdown modal still overlays Media mode when triggered.
- Mode persists in NVS — but if Orion is offline at boot, device starts in Calendar regardless (toggle hidden until Orion reconnects).
- **On PC disconnect during Media mode:** firmware auto-reverts to Calendar + hides toggle. The only non-user path to exit Media mode.
- Exiting to Calendar restores the priority-ordered state machine in `state-machine.md`.

## Out of scope

- BLE payloads and OS bridge flow → `ble-protocol.md` §12
- Orion shortcut configuration UI → `pc-app.md`
- Additional album-art gestures (long-press, pinch, two-finger) — reserved for future use; only tap + swipe-left/right/up-down are wired in v1
