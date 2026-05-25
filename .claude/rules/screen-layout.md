# Ori — Persistent Screen Layout

## Overall Structure

```
┌─────────────────────────────────────────────────────────────┐
│ STATUS BAR  (always visible — hidden only during setup flow) │
│  14:30 · Wed, May 14   [gmail][msg]   [phone-broken-icon?]  │
├───────────────────────────────────┬─────────────────────────┤
│                                   │                         │
│   LEFT PANEL  (dynamic, 2/3)      │  RIGHT PANEL (1/3)      │
│                                   │  • Profile photo        │
│                                   │  • Full name            │
│                                   │  • Job title            │
│                                   │                         │
└───────────────────────────────────┴─────────────────────────┘
```

Pixel dimensions: 800 × 480. Left panel: 528 px wide. Right panel: 269 px wide. Divider: 3 px.

## Status Bar

- **Always visible** in every runtime state. The only exception is the first-time setup flow and the runtime re-pair phone screen (hidden to match setup layout exactly).
- Height: 84 px.
- Contents left → right:
  - Date and time (always current local time), **except** on the after-hours digital clock screen where it is hidden to avoid redundancy.
  - ANCS notification icons — right-to-left order. Tap an icon to open a full-screen detail overlay (title + message body); dismissed via the **Close** button only. No replying — ANCS is read-only.
  - Phone-disconnect icon — shown **only** when the phone is not connected over BLE.
  - **Mode toggle button** — rightmost element at all times when visible. Switches the left panel between calendar mode (default) and Controls mode. **Hidden when Orion is offline** (no BLE link to PC) — Controls mode is useless without the Orion bridge, so the toggle simply disappears from the status bar. See `keyboard-mode.md`.

## Right Panel — Profile Card

Always visible (in both calendar and Controls modes). Contents:
- Circular profile photo (or Ori brand mark if no photo yet) — **the border colour reflects the user's Microsoft Teams presence** (see below)
- Full name — **single line, ellipsis on overflow**
- Job title — **single line, ellipsis on overflow**

Data is pulled from the Orion PC app during initial pairing, stored in flash, and persists across power cycles and connection loss.

### Profile-photo border — Teams presence indicator

The 6 px border around the circular profile photo encodes the user's Microsoft Teams presence as a glanceable colour, pushed by Orion via the BLE Presence Status characteristic (`ble-protocol.md` §3 char 16):

| BLE byte | State | Border colour | Comes from Teams state… |
|---|---|---|---|
| `0x00` | Available | Teams green `#92C353` | Available |
| `0x01` | Busy | Teams red `#C4314B` | Busy, Do Not Disturb, In a call, In a meeting, Presenting |
| `0x02` | Away | Teams amber `#FFAA44` | Be Right Back, Appear Away |
| `0x03` | Offline | Teams grey `#8A8884` | Appear Offline, unknown, **or** the device-side fallback when Orion is BLE-disconnected |

Swatches match Microsoft Teams' actual presence colors — chosen for instant cross-app recognition, not derived from Ori's calm palette.

**Device-side fallback rule:** when no BLE link to Orion is up, Ori renders the border in `#8A8884` (Teams Offline grey — same as the `0x03` table entry above) regardless of the last cached value Orion pushed. The device must never claim a presence it can't currently verify — a stale green border showing "Available" while Orion is actually offline would be a lie.

Border colour transitions are animated (~300 ms ease) so changes feel smooth rather than jarring.

**Name and title are rendered single-line.** If a string is longer than the right panel can fit, the firmware truncates with an ellipsis (`Christopher Vandenbe…`) rather than wrapping to a second line — wrapping would push the layout downward and break the calm-glanceable feel. This truncation is a defensive safety net only; Orion enforces stricter display-friendly limits at input time (name ≤ 24 chars, title ≤ 40 chars — see `pc-app.md`) so the device never has to truncate under normal use.

## Left Panel

Dynamic. The content depends on the current top-level mode (selected by the status-bar mode-toggle):

- **Calendar mode** — content selected by the priority-ordered state machine in `state-machine.md` (meeting list / clock / PTO / countdown / reconnect overlay).
- **Controls mode** — the media-controller UI (large album-art image with tap/swipe gestures for transport and volume, currently-playing metadata, three user-assignable shortcuts). Only available when Orion is online. See `keyboard-mode.md`.
