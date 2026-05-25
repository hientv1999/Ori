# Ori — Setup Flow and Factory Reset

## First-Time Setup

Shown on first boot or after a factory reset. The status bar is **hidden** during the entire setup flow to maximize screen height.

A 4-dot progress indicator is shown at a fixed Y position near the bottom of every setup screen (Welcome through Step 4). The dot row must not move between pages.

### Welcome and Step 1 layout rules (locked design)

Both screens share the same brand-mark + title + description + primary-button stack, top-aligned (flex `START` on main axis) within the content area (`pad_top 104 px` = status-bar-height 84 + design-padding 20, `pad_bottom 70 px`). `LV_OBJ_FLAG_OVERFLOW_VISIBLE` is set on the content container so the logo's ambient glow can bleed above the padded top edge.

| Element | Spec |
|---|---|
| Brand mark — glow blob | Soft circular glow, ~256 px diameter, centred on the logo ring; LVGL shadow on a transparent circle |
| Brand mark — logo ring | 132 × 132 px context; outer ring 170 px (1px, 30% opacity), inner ring 145 px (1px, 20% opacity), main ring 120 px (4px, full opacity + tight halo shadow), inner dot 37 px |
| Brand mark — "o r i" wordmark | 30 px below the ring bottom; flanking gradient lines; 'r' coloured accent |
| Brand mark → h2 gap | 4 px (`pad_top` on h2) |
| h2 title | `font_display` (Montserrat **42 px**), weight 300 |
| h2 → description gap | 8 px (`pad_top` on `p`) |
| Description | `font_meta` (Montserrat 22 px), secondary colour, max-width 620 px, wraps freely |
| Description → button gap | 24 px spacer — **balances** the gap above the button with the gap below it to the dot row |
| Primary button (Start / Next) | `pad_v 36 px`, `pad_h 54 px`, 20 px weight 600, **uppercase**, letter-spacing 2.5 px, accent fill, **`border-radius: 999 px` (full pill)** |
| Step dots | `position: absolute`, fixed at y = 440 px (firmware: `DOT_ROW_Y = 440`) |

**Why top-aligned?** With `LV_FLEX_ALIGN_CENTER` (or CSS `justify-content: center`), a longer description on Step 1 shifts the brand-mark upward relative to Welcome because the total content height differs. Pinning to `START` keeps the logo at the same screen-top position on every screen regardless of content below it.

| Screen | Dot state | Notes |
|---|---|---|
| Welcome | All 4 dots inactive | Shows Ori brand mark; "Start" button advances |
| Step 1 — Install Orion | Dot 0 active | Visit ori.app/orion on PC (Windows or macOS); user taps Next after install |
| Step 2 — Orion pairing | Dot 1 active | Ori shows BLE name + spinning animation; passkey modal appears on top when Orion connects |
| Step 3 — Orioning | Dot 2 active | Progress ring while Orion syncs data (profile, calendar, PTO, time) |
| Step 4 — Phone pairing | Dot 3 active | Optional — user may skip; pairing can be done later via long-press |
| Setup complete | Dots **hidden** | Brief acknowledgement before transitioning to normal state |

### Passkey Modal (Step 2)
- A 6-digit BLE passkey is displayed in a modal popup on top of the Step 2 screen.
- User must confirm the code matches what Orion shows on PC (secure BLE bonding).
- Step 2 completes only when Orion is running and synced.

### Setup Failure Rules
- If PC pairing fails, stay on that step and allow retry.
- The user cannot go backward through setup steps.
- The only alternative exit is factory reset (long-press profile photo).
- Phone pairing is optional; user may skip and pair later.

## Factory Reset

- **Trigger**: long-press the circular profile photo for 3 seconds from **any** state.
- Shows a confirmation popup with Cancel and Reset actions.
- Reset erases: profile, meetings, PTO, pairing bonds, and saved brightness.
- Device returns to first-boot setup state.

## Runtime Re-Pair Phone

- **Trigger**: long-press the phone-disconnect icon in the status bar for 3 seconds.
- Available from every runtime state (not during first-boot setup).
- The status bar is **hidden** during re-pair so the layout is identical to Step 4 (same spinner, same room).
- A Cancel button returns to the main screen.
