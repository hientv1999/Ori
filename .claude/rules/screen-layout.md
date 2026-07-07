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
  - **Date and time** — tappable: single tap enters Clock view; long-press enters Calendar (month view). Hidden on the Clock and Calendar screens themselves, and **hidden entirely when there is no local time yet** (e.g. after a cold power cycle, before Orion or the iPhone supplies the clock) — no "--:--" placeholder (and consequently no way to reach Clock or Calendar until a clock source connects). Time comes from Orion (primary) or the iPhone's Current Time Service (backup); see `connectivity.md`.
  - ANCS notification icons — up to **5 visible** (layout hard cap; 6 would overflow the bar). Right-to-left order. Queue depth is 50 (each entry's full detail is cached in PSRAM, not SRAM) — notifications beyond the 5th are hidden but shift left as earlier ones are dismissed. Tap opens a full-screen detail overlay (title + body); **Close** button only. No replying — read-only.
  - **Phone icon** — always visible. Neutral colour when the iPhone is connected, danger red when disconnected (no slash glyph). Tap: connected → Unpair iPhone modal (shows the phone's GAP device name); disconnected → re-pair iPhone screen (stale bond wiped automatically).
  - **Mode-toggle button** — rightmost element always. Cycles left panel between **Calendar** and **Controls**. **Hidden when Orion is offline** — except in Clock or Calendar view, where it acts as a return button (calendar icon, neutral style). Icon always shows the destination. See `media-mode.md`.

## Right Panel — Profile Card

Always visible. Contents:
- Circular profile photo, **228 × 228 px** (or Ori wordmark if no photo) — **border colour reflects Teams presence** (see below)
- Full name — single line, ellipsis on overflow (Orion enforces ≤ 32 chars at input)
- Job title — single line, ellipsis on overflow (Orion enforces ≤ 32 chars at input)
- Weather badge (top-left corner, outside the photo) + temperature bubble (bottom-right corner, outside the photo) — see below

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

### Weather badge + temperature bubble

Two small elements hang just outside the profile-photo circle, pushed by Orion via the Device Settings characteristic (`"w"`/`"d"` fields — `ble-protocol.md` §3/§4/§6.4):

- **Weather badge** — 46 px circle, top-left corner, `--screen-elev` background + a thin border, holding a condition glyph. Seven conditions: Clear, Partly Cloudy, Cloudy, Rain, Thunderstorm, Snow, Fog. Every glyph is built from plain circles, rounded rects, and line strokes (no bezier art) so it maps directly to LVGL primitives (`lv_obj` circles/rounded-rects, `lv_line`) — no bitmap asset needed. Reference implementation: `WEATHER_ICONS` in `Ori_UI_Prototype.js`.
- **Temperature bubble** — white pill, bottom-right corner, whole-number degrees Fahrenheit with a `°` suffix (e.g. "72°").

**Device-side fallback:** same policy as presence — before the first Device Settings write containing `"w"`/`"d"`, and whenever the BLE-PC link is down, both elements are hidden entirely (no placeholder glyph, no stale reading). They reappear once Orion reconnects and re-pushes.

## Left Panel

Dynamic, driven by the mode-toggle:

- **Calendar mode** — priority-ordered state machine content (meeting list / Time Off / countdown / reconnect overlay). See `state-machine.md`.
- **Controls mode** — media-controller UI. Only available when Orion is online. See `media-mode.md`.
