# Ori — Persistent Screen Layout

## Overall Structure

```
┌──────────────────────────────────────────────────────────────────┐
│ STATUS BAR  (always visible — hidden only during setup flow)     │
│  14:30 · Wed, May 14   [gmail][msg]   [phone]  [toggle]         │
├────────────────────────────────────────┬─────────────────────────┤
│                                        │                         │
│   LEFT PANEL  (dynamic, 2/3)           │  RIGHT PANEL (1/3)      │
│                                        │  • Profile photo        │
│                                        │  • Full name            │
│                                        │  • Job title            │
│                                        │                         │
└────────────────────────────────────────┴─────────────────────────┘
```

Pixel dimensions: 800 × 480. Left panel: 528 px wide. Right panel: 269 px wide. Divider: 3 px.

## Status Bar

- **Always visible** in every runtime state. Hidden only during first-time setup flow and the runtime re-pair iPhone screen.
- Height: 84 px.
- Contents left → right:
  - **Date and time** — tappable: single tap enters Clock view; long-press enters Calendar (month view). Hidden on the Clock and Calendar screens themselves, and **hidden entirely when there is no local time yet** (e.g. after a cold power cycle, before Orion or the bonded phone supplies the clock) — no "--:--" placeholder (and consequently no way to reach Clock or Calendar until a clock source connects). Time comes from Orion (primary) or the bonded iPhone/iPad's Current Time Service (backup); see `connectivity.md`. Rendered in 24-hour ("14:30") or 12-hour ("2:30 PM") per the user's time-format preference, set by Orion over BLE (`ble-protocol.md` Device Settings key `"h"`, default 24-hour); the same preference governs the clock faces, meeting-list times, and ANCS timestamps.
  - ANCS notification icons — up to **5 visible** (layout hard cap; 6 would overflow the bar). Right-to-left order. Queue depth is 100 (each entry cached in PSRAM) — notifications beyond the 5th are hidden but shift left as earlier ones are dismissed. Tap opens a full-screen detail overlay (title + body); **Close** button only, read-only. **Call tile ring:** a call notification's icon tile gets a solid 3 px ring — yellow (`theme::COLOR_CALL_RINGING`) while ringing/unanswered, red (`theme::COLOR_DANGER`) the instant it's answered/active (`widget_status_bar.cpp`'s `make_ancs_tile`, driven by `modal_incoming_call::session_state()`). No pulse/animation. Orion mirrors this exact color pair on its own header call chip (`pc-app.md`).
  - **Phone icon** — always visible. Always the neutral secondary-text colour in both states — a diagonal slash is drawn across the glyph when disconnected, rather than a colour swap, so the icon reads the same whether or not the viewer can distinguish red from grey (design revised 2026-07; `memory.md`). Tap: connected → Unpair modal (title reads "Unpair iPhone"/"Unpair iPad" once the bonded device's model is known, else the generic "Unpair iPhone or iPad" — `connectivity.md` §2; shows the phone's GAP device name); disconnected → re-pair screen (stale bond wiped automatically).
  - **Mode-toggle button** — rightmost element always. Cycles left panel between **Calendar** and **Controls**. **Hidden when Orion is offline** — except in Clock or Calendar view, where it acts as a return button (calendar icon, neutral style). Icon always shows the destination. See `media-mode.md`.

## Right Panel — Profile Card

Always visible. Contents:
- Circular profile photo, **228 × 228 px** (or Ori wordmark if no photo) — **border colour reflects Teams presence** (see below)
- Full name — single line, ellipsis on overflow (Orion enforces ≤ 32 chars at input)
- Job title — single line, ellipsis on overflow (Orion enforces ≤ 32 chars at input)
- Weather icon (top-left corner, outside the photo) + temperature text (top-right corner, outside the photo) — see below

Data pulled from Orion during initial pairing, stored in flash, persists across power cycles and connection loss.

### Profile-photo border — Teams presence indicator

6 px border + a static (non-animated) soft glow in the same colour encodes the user's Teams presence, pushed by Orion via the Presence Status characteristic (`ble-protocol.md` §3). **Offline shows the border with no glow** — a glow on the grey/no-signal state would read as a render glitch rather than a deliberate status:

| BLE byte | State | Border colour | Teams state |
|---|---|---|---|
| `0x00` | Available | `#92C353` | Available |
| `0x01` | Busy | `#C4314B` | Busy, Do Not Disturb, In a call, In a meeting, Presenting |
| `0x02` | Away | `#FFAA44` | Be Right Back, Appear Away |
| `0x03` | Offline | `#8A8884` | Appear Offline, unknown, or BLE-disconnected fallback |

**Device-side fallback:** when the BLE-PC link is down, force `#8A8884` regardless of the last cached value — never claim a presence that can't be verified.

Border colour transitions animate at ~300 ms ease.

### Weather icon + temperature text

Two small elements sit just outside the profile-photo circle (and its presence ring) — no bubble/background on either, and both are positioned clear of the photo so they never overlap it — pushed by Orion via the Device Settings characteristic (`"w"`/`"d"`/`"u"` fields — `ble-protocol.md` §3/§4/§6.4):

- **Weather icon** — top-left corner, bare condition glyph with no background or border. Seven conditions: Clear, Partly Cloudy, Cloudy, Rain, Thunderstorm, Snow, Fog. Every glyph is built from plain circles, rounded rects, and line strokes (no bezier art) so it maps directly to LVGL primitives (`lv_obj` circles/rounded-rects, `lv_line`) — no bitmap asset needed. Reference implementation: `WEATHER_ICONS` in `Ori_UI_Prototype.js`.
- **Temperature text** — top-right corner, plain white text, no background or border, whole-number degrees with a `°` suffix followed by the unit letter Orion declared via `"u"` (e.g. "72°F" or "22°C"). Ori never converts between units — it just renders whichever integer + unit Orion sends (`ble-protocol.md` §4).

**Device-side fallback:** same policy as presence — before the first Device Settings write containing `"w"`/`"d"`/`"u"`, and whenever the BLE-PC link is down, both elements are hidden entirely (no placeholder glyph, no stale reading). They reappear once Orion reconnects and re-pushes.

## Left Panel

Dynamic, driven by the mode-toggle:

- **Calendar mode** — priority-ordered state machine content (meeting list / Time Off / countdown / reconnect overlay). See `state-machine.md`.
- **Controls mode** — media-controller UI. Only available when Orion is online. See `media-mode.md`.
