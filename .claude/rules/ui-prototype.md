---
paths:
  - "Ori_UI_Prototype.html"
  - "Ori_UI_Prototype.js"
---

# Ori — UI Prototype

`Ori_UI_Prototype.html` + `Ori_UI_Prototype.js` is a browser-based simulator of all screens at 800 × 480 px inside a bezel mockup. Open directly in any modern browser — no build step, no server.

- **`Ori_UI_Prototype.html`** — layout shell, CSS variables, SVG icon sprites, loads the JS.
- **`Ori_UI_Prototype.js`** — all screen definitions, data, HTML renderers, and interaction logic.

## Key JS Constants (top of file)

| Constant | Purpose |
|---|---|
| `PROFILE` | `{ name, title }` shown in the right panel |
| `BLE_NAME` | Device BLE advertised name shown during setup/pairing screens |
| `PASSKEY` | 6-digit passkey string shown in the Step 2 modal |
| `TODAY_MEETINGS` | Meeting data for the default meeting-list screen |
| `OVERLAP_MEETINGS` | Overlapping-meetings edge case |
| `LONG_TITLE_MEETINGS` | Long-title wrap edge case |
| `OVERLAP_LONG_MEETINGS` | Overlap + long title edge case |
| `LONG_LIST_MEETINGS` | Scrollable-list edge case |

## Screen System

All screens are declared in the `SCREENS` object. Each entry defines:
- `label`, `title`, `desc` — sidebar metadata
- `statusBar` — `{ ancsApps, phoneConnected, hideDateTime }` for the status bar
- `leftRender()` — returns HTML for the left panel
- `setup()` — returns HTML for a full-screen setup overlay (hides body)
- `modal()` — returns HTML for a modal scrim overlay
- `hideStatusBar` — boolean, hides the status bar entirely

Call `setScreen(id)` to switch the active screen. To add a screen: add an entry to `SCREENS` and a `<button data-screen="your-id">` in the nav sidebar.

## Prototype-Only Behaviors

- The left nav sidebar and bezel are prototype chrome — not on the real device.
- Long-press uses `mousedown` + `setTimeout` (1200 ms default) in `bindLongPress()`.
- Clock auto-refreshes every 30 s via `setInterval`.
- `escapeHtml()` wraps all user-data strings to prevent XSS in the prototype.
