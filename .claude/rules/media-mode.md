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
| |                                      |  |  touch   = reveal controls
| |                  ART                 |  |  tap ⏯   = play/pause
| |                                      |  |  swipe h = prev / next
| +--------------------------------------+  |  swipe v = volume + HUD
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
| **Plain tap on the art** | Resolved only after `DOUBLE_TAP_MS` (400 ms) with no follow-up tap in the same half: reveals the controls if they started hidden, dismisses them (skipping the 5 s wait) if they were already showing | < 20 px in both axes | Nothing happens until the window closes with no follow-up — see "Deferred tap resolution" below |
| **Tap the circular play/pause button** | `{op:"play_pause"}` — the ONLY way to LOCALLY toggle playback | < 20 px in both axes, inside the button's circular hit area | Icon swaps between the play triangle and the pause bars; also (re)starts the 5 s auto-hide countdown, same as any reveal |
| **Double-tap — left half** | `{op:"seek", arg:position_s}` — jumps the track **backward** by the configured seek step. **Never reveals the button/bar if they started hidden** — only extends their countdown if they were already visible | two taps in the left half within 400 ms (`DOUBLE_TAP_MS`) | The seek-flash (below) always shows regardless of button/bar visibility. If the bar happens to be visible, its fill/thumb/timestamp jump to the new position too, same as a drag-seek |
| **Double-tap — right half** | `{op:"seek", arg:position_s}` — jumps the track **forward** by the configured seek step. Same non-reveal rule as the left half | mirror of left-half double-tap | same as above, "+Ns" label |
| **Swipe right** | `{op:"next"}` — does **not** touch the button/bar controls' visibility either way (shown stays shown, hidden stays hidden) | > 50 px horizontal, \|dx\| > \|dy\| | Art shifts right ~40 px then snaps back |
| **Swipe left** | `{op:"prev"}` — same non-interference | mirror of swipe right | Art shifts left ~40 px then snaps back |
| **Vertical swipe** | `{op:"vol_set", arg:N}` on release — only the (separate) volume HUD is affected; the button/bar controls are left exactly as they were | > 25 px vertical, \|dy\| > \|dx\| | Volume HUD (bar + %) fades in, tracks live, lingers ~800 ms |
| **New media metadata, album art, or total playtime (duration) changes** | Reveals the controls (scrim + button + progress bar, if seek-eligible) — a genuinely new track, new art, or a real duration change; never an Orion play/pause echo (see below). **A play-position-only change never reveals** — not an Orion-side seek, not Ori's own internal per-second position tick, not Ori's own local drag-/double-tap-seek gestures | n/a — driven by BLE, not touch | Same instant reveal as any other trigger |

Volume sensitivity: ~400 px swipe = full 0..100 range (firmware `V_SENS_NUM`/`V_SENS_DEN` in `screen_media_mode.cpp`). Swipe up = louder. Orion applies volume via OS API and writes back `Host Volume State`; Ori ignores incoming pushes while a swipe is in progress (drag-wins override — see `ble-protocol.md` §12). Volume HUD bar: background is 65% opaque white (`HUD_BAR_BG_OPA`), fill is a darker gold (`theme::COLOR_ACCENT_DARK`, ~65% brightness of the standard accent) rather than the usual accent color — deliberately darker/more saturated than every other accent-colored fill in the UI so it doesn't wash out against the album art. Fill stays fully opaque.

**Externally-driven volume changes also surface the HUD.** A `Host Volume State` push that's NOT Ori's own swipe echoing back (i.e. the user changed volume via the OS mixer, another app, a hardware key — anything other than Ori's own swipe gesture) shows the HUD as a momentary toast — even if it wasn't already on screen — so an out-of-band volume change is never silently invisible. Auto-hides after 1.5 s with no further change (`HOST_VOLUME_TOAST_MS`), or immediately if the user starts a real swipe in the meantime (which takes over the HUD itself). Ori's own swipe echo (Orion's write-back confirming a level Ori just swiped to) only refreshes the fill/label — it never re-triggers the toast, since the swipe already showed and hid the HUD around the gesture itself. See `screen_media_mode::update_volume_from_host()`.

**Touch-revealed controls (YouTube-style):** by default the art shows at full brightness with no overlay at all. Revealing shows, all at once: a dark scrim (art dims to 40%, `CONTROLS_DIM_OPA`/`LV_OPA_40`), the progress bar (only if the track is seek-eligible — see below), and a centred circular play/pause button. The scrim applies to both the gradient placeholder and the real album-art image (separate layered objects — both must dim together). **The 5 s auto-hide countdown (`TL_AUTO_HIDE_MS`) always applies** — whether the track is playing or paused, there is no permanently-visible state. Revealing is always instant, never a fade-in.

**Only a plain (non-double) tap and genuine BLE-driven media updates change the controls' visibility — nothing else does, and play POSITION changes never do either.** Swiping (horizontal or vertical) and double-tapping to seek all leave the button/progress-bar controls exactly as they were: if shown, they stay shown; if hidden, they stay hidden. A vertical swipe's volume HUD and a horizontal swipe's next/prev are entirely separate visual systems from the touch-revealed controls. The things that ever change the controls' shown/hidden state are: (1) a plain tap resolving as a lone tap (below), and (2) new media metadata, new album art, or a real total-playtime (duration) change arriving over BLE (below) — everything else, including any play-position-only change (an Orion-side seek included), either extends the existing state (double-tap, if already visible) or does nothing to it at all.

**A plain tap is deferred, not immediate.** A tap on the art might be the first half of a double-tap-to-seek, which must never reveal the button/bar — so a plain tap doesn't resolve on the spot. Touching the art starts a `DOUBLE_TAP_MS` (400 ms) window; if it closes with no follow-up tap in the same half, the tap resolves as a lone tap — **toggling** the controls: revealing if they started hidden, dismissing them (skipping the rest of the 5 s wait) if they were already showing. If a genuine double-tap *does* land within the window, it's resolved as a seek instead (see below) and the pending toggle never fires — no flicker either way. (An earlier version revealed on every touch instantly and dismissed on the first tap of a pair immediately; both caused a visible flash right before a double-tap-seek resolved — this deferral is what fixes both.)

**Double-tap-seek never reveals hidden controls.** Because the button/progress bar might currently be hidden, and a double-tap's job is to seek — not to summon the controls — a double-tap-seek explicitly does NOT reveal them if they started hidden. If they're already visible, the seek still extends their 5 s countdown (the one exception to "leaves it exactly as it was" — the shown/hidden state itself still never changes). Either way, the seek-flash dim+label feedback always shows — it's independent of the button/bar's own visibility.

**New media metadata, album art, or total playtime reveals the controls — but only for a genuine change, and NEVER for a position-only change.** Same treatment as YouTube surfacing its scrubber when a new video loads:
- **Track metadata** (`MediaMetadata`'s title/artist, `ble-protocol.md` §4/§6.4): gated on the title actually changing (the same check that already clears the previous album art on a track change) — Orion also resends `MediaMetadata` on every local play/pause toggle with the SAME title unchanged, and that must NOT reveal the controls (see the externally-driven play/pause rule below); only a real track change does.
- **Album art** (`Media Album Art` char, chunked JPEG): reveals once a newly-streamed image is fully decoded and swapped in, and also when art is explicitly cleared (reverted to the gradient placeholder) — both are handled by the same `clear_album_art()`/`set_album_art()` call sites, so a track change's art-clear and a fresh image's arrival each reveal once, without a redundant second reveal from the metadata path.
- **Total playtime** (`MediaMetadata`'s `"d"` field, duration): reveals only when the incoming duration genuinely differs from what Ori already has tracked — e.g. a new track's duration becoming known.
- **Play position** (`MediaMetadata`'s `"o"` field): deliberately **excluded** from the reveal check entirely — a position-only change (whether Ori's own internal 1-second dead-reckoning tick between BLE pushes, Ori's own local drag-seek/double-tap-seek gestures, or a genuine seek on Orion itself) never reveals or otherwise changes the controls' visibility. Only a duration change does. (Ori's internal tick also bypasses this reveal check by construction — see `screen_media_mode.cpp`'s `apply_seek_visual()`/`g_pos_timer` — but position is excluded from the comparison regardless of source.) An Orion play/pause-only toggle resends the SAME duration unchanged, so it correctly does not reveal either.

**Dismissal fades the button and progress bar; the scrim snaps.** Whenever the controls hide — whether the 5 s timer expires or a deferred tap resolves as a dismissal — the button and progress bar each fade out over ~400 ms (`TL_FADE_OUT_MS`), the same treatment, so the button doesn't look inconsistent next to the bar. The scrim (album-art dim) snaps back to full brightness instantly.

**Circular play/pause button:** a 92 px circle, filled black at 50% opacity (a translucent "chip" behind the icon, same treatment as YouTube's own control — not accent-colored, so it stays neutral against any album art), centred on the art. Contains a plain white icon — a triangle when paused (tap resumes) or two vertical bars when playing (tap pauses) — swapping live with playback state. Hidden together with the rest of the controls; tapping it is the **only** way to LOCALLY toggle playback. Since the button is only tappable once already revealed, tapping it always (re)arms the 5 s auto-hide countdown rather than performing a fresh reveal.

**Externally-driven (Orion) play/pause changes never touch the controls' visibility.** When Orion pushes a play/pause change via `MediaMetadata`'s `"p"` field (§4/§6.4 of `ble-protocol.md`) — as opposed to a local button tap — only the button's icon updates (so it shows correctly whenever the controls are next revealed); the controls are never shown or hidden as a side effect. If they're already hidden, they stay hidden.

**Double-tap-to-seek zones:** the art is split into two equal horizontal halves (242 px each of the 484 px width) — there is no separate middle play/pause zone anymore, since the circular button now owns that job and sits on top of the gesture surface (a tap landing on the button is captured by the button itself and never reaches the half-based dispatch below). Only two taps landing in the *same* left/right half within the double-tap window (400 ms) count as a double-tap and fire the seek; a single tap in either half arms the deferred tap resolution described above instead. No-op on a track that isn't seek-eligible (same `can_seek`+duration gate as the drag-seek bar below). **Seek step is Orion-configurable** (`DeviceSettings` char `000E` key `"k"`, `ble-protocol.md` §4/§6.4), 1-60 seconds, NVS-persisted, default **10 s**.

**Playtime progress bar (seek):** a 46 px overlay at the bottom of the art — track, accent playhead fill, thumb dot, current/duration timestamps — shown only when Orion reports `can_seek:true` with a valid duration (`MediaMetadata`'s `"c"`/`"o"`/`"d"`), AND only while the controls above are revealed. Drag to scrub live; release emits `{op:"seek", arg:seconds}`. Becoming ineligible (non-seekable track, or nothing playing) hides the bar immediately even if the rest of the controls stay up — a later eligible track shows the bar again next time the controls are revealed.

**Nothing playing:** slot shows a muted dark radial gradient with the **Ori wordmark** centred. Title = "Nothing playing"; artist = em-dash. Gestures still emit commands (Orion may interpret a tap to wake/resume).

**Loading:** a dim (not opaque) black veil covers the entire art — the default/current art stays dimly visible underneath, not hidden — the moment Orion actually starts streaming a new `Media Album Art` transfer (the first chunk fragment lands — not merely on entering Controls mode or a track change with no art yet incoming). The non-dimmed (full-opacity) area then rises from the bottom edge upward as real progress (from the chunk frame's `seq`/`total_frags`) comes in. At 100% the veil is fully gone — the entire art reads at full opacity — right as the real decoded image swaps in, so the old/gradient art is never left fully hidden mid-transfer. Superseded by a track change (new metadata clears the in-progress veil too) or a failed/NACK'd transfer. See `screen_media_mode::show_art_loading()` / `update_art_loading_progress()` / `hide_art_loading()`.

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
- Additional album-art gestures (long-press, pinch, two-finger) — reserved for future use; only tap, double-tap (left/right halves), and swipe-left/right/up-down are wired in v1
