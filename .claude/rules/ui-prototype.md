---
paths:
  - "Ori_UI_Prototype.html"
  - "Ori_UI_Prototype.js"
---

# Ori — UI Prototype

`Ori_UI_Prototype.html` + `Ori_UI_Prototype.js` is a browser-based simulator of all screens rendered at 800 × 480 px inside a bezel mockup. Open directly in any modern browser — no build step, no server needed.

## Structure

- **`Ori_UI_Prototype.html`** — layout shell, CSS variables, SVG icon sprites, loads the JS.
- **`Ori_UI_Prototype.js`** — all screen definitions, data, HTML renderers, and interaction logic.

## Key JS Constants (Top of File)

| Constant | Purpose |
|---|---|
| `PROFILE` | `{ name, title }` shown in the right panel |
| `BLE_NAME` | Device BLE advertised name shown during setup/pairing screens |
| `PASSKEY` | 6-digit passkey string shown in the Step 2 modal |
| `TODAY_MEETINGS` | Meeting data for the default meeting-list screen |
| `OVERLAP_MEETINGS` | Meeting data for the overlapping-meetings edge case |
| `LONG_TITLE_MEETINGS` | Meeting data for the long-title wrap edge case |
| `OVERLAP_LONG_MEETINGS` | Meeting data for the overlap + long title edge case |
| `LONG_LIST_MEETINGS` | Meeting data for the scrollable-list edge case |

## Screen System

All screens are declared in the `SCREENS` object. Each entry defines:
- `label`, `title`, `desc` — sidebar metadata
- `statusBar` — `{ ancsApps, phoneConnected, hideDateTime }` for the status bar
- `leftRender()` — function returning HTML for the left panel
- `setup()` — function returning HTML for a full-screen setup overlay (hides body)
- `modal()` — function returning HTML for a modal scrim overlay
- `hideStatusBar` — boolean, hides the status bar entirely

Call `setScreen(id)` to switch the active screen programmatically.

## Adding a New Screen

1. Add an entry to `SCREENS` with the appropriate keys.
2. Add a `<button data-screen="your-id">` in the nav sidebar in the HTML.
3. Implement a renderer function if needed.

## Prototype-Only Behaviors

- The left nav sidebar and bezel are prototype chrome — they do not exist on the real device.
- Long-press interactions use `mousedown` + `setTimeout` (1200 ms by default) in `bindLongPress()`.
- The clock auto-refreshes every 30 s via `setInterval`.
- `escapeHtml()` is used for all user-data strings to prevent XSS in the prototype.
