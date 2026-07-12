# Ori — Media mode

Ori has two user-selectable modes via the **status-bar mode-toggle button**:

- **Calendar mode** (default) — meeting list / "No meetings today." Always available. See `state-machine.md`.
- **Media mode** — album-art transport + volume UI for the paired PC. Commands travel as BLE to Orion, which bridges to OS APIs. **Only available when Orion is connected** — auto-reverts to Calendar on PC disconnect. Internal code name: `kbd-mode` (historical; user-facing copy says "Controls").

**Clock** is a third view entered by tapping the status-bar date/time — not part of the toggle cycle. The mode-toggle becomes a return button while in Clock. See `state-machine.md`.

Status bar and profile card are visible in all three modes.

## Mode-toggle button (status bar)

- **Position:** rightmost status-bar element always. Right-cluster order: `[ANCS icons] [phone icon (always visible)] [mode-toggle]`.
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
| +--------------------------------------+  |  Album art 484×216 px
| |                                      |  |  tap     = play/pause
| |                  ART                 |  |  swipe h = prev / next
| |                                      |  |  swipe v = volume + HUD
| +--------------------------------------+  |
|                                            |
|   Industrial Symphony No. 1 - The Dream…  |  Title (24 px, ellipsis)
|         Angelo Badalamenti                 |  Artist (22 px, secondary)
|                                            |
|  +----------+  +----------+  +----------+ |  3 user-assignable
|  |    1     |  |    2     |  |    3     | |  shortcuts
|  +----------+  +----------+  +----------+ |
+--------------------------------------------+
```

### 1. Album art — the interaction surface

484 × 216 px, 14 px corner radius, full usable left-panel width (528 − 22 px padding each side). Vertical budget: 10 + 216 + 76 (title/artist block) + 80 (shortcut row) + 10 = 392 px (4 px headroom in 396 px). Pushed by Orion via `Media Album Art` char on every track change — resized to 484 × 216, JPEG (~15–30 KB), chunked. Ori decodes and PSRAM-caches (not NVS; re-fetched on reconnect).

| Gesture | Action | Threshold | Feedback |
|---|---|---|---|
| **Tap** | `{op:"play_pause"}` — but see the progress-bar note below: on a seek-eligible track, the FIRST tap only reveals the bar and does not toggle playback | < 20 px in both axes | Play/pause icon flash (~550 ms, scales + fades) |
| **Swipe right** | `{op:"next"}` | > 50 px horizontal, \|dx\| > \|dy\| | Art shifts right ~40 px then snaps back |
| **Swipe left** | `{op:"prev"}` | mirror of swipe right | Art shifts left ~40 px then snaps back |
| **Vertical swipe** | `{op:"vol_set", arg:N}` on release | > 25 px vertical, \|dy\| > \|dx\| | Volume HUD (bar + %) fades in, tracks live, lingers ~800 ms |

Volume sensitivity: ~400 px swipe = full 0..100 range (firmware `V_SENS_NUM`/`V_SENS_DEN` in `screen_media_mode.cpp`). Swipe up = louder. Orion applies volume via OS API and writes back `Host Volume State`; Ori ignores incoming pushes while a swipe is in progress (drag-wins override — see `ble-protocol.md` §12).

**Playtime progress bar (seek):** a 46 px overlay at the bottom of the art — track, accent playhead fill, thumb dot, current/duration timestamps — shown only when Orion reports `can_seek:true` with a valid duration (`MediaMetadata`'s `"c"`/`"o"`/`"d"`). Drag to scrub live; release emits `{op:"seek", arg:seconds}`. **Hidden by default** even when eligible — any touch on the art reveals it, hiding again after **5 s** of no further touch (`TL_AUTO_HIDE_MS`); dragging past 5 s keeps resetting the countdown. Becoming ineligible (non-seekable track, or nothing playing) drops any pending reveal — a later eligible track always starts hidden.

**Tap is two-step once seek-eligible:** the first tap on a hidden bar only reveals it, without toggling play/pause; only once visible does a tap toggle playback (and refresh the reveal countdown) — avoids both firing off one tap. Non-seekable tracks are unaffected — tap toggles play/pause immediately.

**Paused:** art dims to ~55% with a centred play-triangle overlay, applied to both the gradient placeholder and the real album-art image (separate layered objects — both must dim together, `apply_paused_visual()`).

**Nothing playing:** slot shows a muted dark radial gradient with the **Ori wordmark** centred. Title = "Nothing playing"; artist = em-dash. Gestures still emit commands (Orion may interpret a tap to wake/resume).

### 2. Title + artist

Centred, ~10 px below art. Single-line ellipsis only — no wrapping.

- **Title:** 24 px (`font_title`), weight 500, primary text. Box height matches the font's real line height (34 px) so descenders can't overflow into the artist line below. Source: `MediaMetadata.title`.
- **Artist:** 22 px (`font_meta`), secondary text, directly below title with no added gap — each label's box height matches its font's real line height (34 px title / 32 px artist), which on its own gives clean separation without overlap. Source: `MediaMetadata.artist`.

### 3. Shortcut row

Three icon-only square buttons, 72 px tall (was 82 px — shrunk to make room for the taller title/artist boxes above), 14 px gap, full row width (~152 px wide each in firmware). 8 px top padding, 10 px bottom padding. Each emits `KeyboardCommand{op:"shortcut", arg:1|2|3}`; Orion runs the configured action.

**Icon assets are compiled into firmware flash.** Orion's settings UI lets the user pick one per slot and writes the selection to Ori as an icon token over Device Settings (`ble-protocol.md` §3/§12). Adding new icon types requires a firmware update — no runtime asset delivery path.

**Unrecognized token → hide the slot.** A token not matching any compiled-in icon (Orion/firmware drift) hides that slot's button entirely rather than a placeholder — remaining valid slots stay centred. The sync itself doesn't reject unknown tokens; only rendering treats them as absent.

Supported icon tokens (14 total):

| Token | Action |
|---|---|
| `vol-mute` | Toggle OS master mute |
| `mic-mute` | Toggle system microphone |
| `screenshot` | Snip Tools (display label — wire token stays `screenshot`, renamed 2026-07-11) |
| `lock-screen` | Lock screen |
| `favorite-1` | User-configured custom action, slot 1 (set in Orion settings) |
| `favorite-2` | User-configured custom action, slot 2 (set in Orion settings) |
| `favorite-3` | User-configured custom action, slot 3 (set in Orion settings) |
| `calculator` | Launch the OS calculator app |
| `copy` | Ctrl+C |
| `cut` | Ctrl+X |
| `paste` | Ctrl+V |
| `undo` | Ctrl+Z |
| `redo` | Ctrl+Y (Windows convention — not Ctrl+Shift+Z) |
| `save` | Ctrl+S |

Favorite is three independently-configured tokens rather than one — each carries its own keyboard combo, so up to three distinct custom actions can be live across the three shortcut slots at once. Since the device shows no text label, the three favorite icons are visually distinguished by a number rendered inside the star glyph (compiled-in asset, same treatment as every other icon here — `firmware/img/shortcut_icons/convert_shortcut_icons.py`).

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
