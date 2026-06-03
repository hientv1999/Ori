# Ori — Setup Flow and Factory Reset

## First-Time Setup

Shown on first boot or after a factory reset. The status bar is **hidden** for the entire flow.

A **3-dot** progress indicator is anchored at a fixed Y position near the bottom of every setup screen (Welcome through Step 3). The dot row must not move between pages.

### Screen sequence

| Screen | Dot state | Notes |
|---|---|---|
| Welcome | All 3 dots inactive | Ori wordmark; "Start" button advances |
| Step 1 — Install Orion | Dot 0 active | Visit `ori.app/orion` on PC; user taps Next after install |
| Step 2 — Link Orion | Dot 1 active | Base: BLE name + spinner. **Passkey modal** overlays when Orion connects. **Orioning modal** overlays during sync (progress ring, "A busy day ahead…"). Both modals dismissed automatically by state transitions — not by the user. |
| Step 3 — iPhone pairing | Dot 2 active | Optional; user may skip; pairing can be done later via long-press |
| Setup complete | Dots **hidden** | Brief acknowledgement before transitioning to normal runtime |

### Welcome and Step 1 layout (locked design)

Both screens share the same wordmark + title + description + primary-button stack, **top-aligned** (`pad_top 104 px` = status-bar-height 84 + 20; `pad_bottom 70 px`). Top-aligned (not centred) so the wordmark sits at the same screen position on every screen regardless of description length.

| Element | Spec |
|---|---|
| Wordmark — "o r i" | Flanking gradient lines; 'r' in accent colour |
| Wordmark → h2 gap | 4 px (`pad_top` on h2) |
| h2 title | `font_display` (Montserrat 42 px), weight 300 |
| h2 → description gap | 8 px (`pad_top` on `p`) |
| Description | `font_meta` (Montserrat 22 px), secondary colour, max-width 620 px, wraps freely |
| Description → button gap | 24 px spacer |
| Primary button (Start / Next) | `pad_v 36 px`, `pad_h 54 px`, 20 px weight 600, uppercase, letter-spacing 2.5 px, accent fill, border-radius 999 px (full pill) |
| Step dots | Fixed at `DOT_ROW_Y = 440` px |

### Passkey modal (Step 2)

- 6-digit BLE passkey displayed in a modal overlay on top of the Link Orion base screen.
- User confirms the code matches what Orion shows on PC (LE Secure Connections passkey entry).
- Dismissed automatically when bonding completes; the Orioning modal then appears on the same base screen.

### Setup failure rules

- If PC pairing fails, stay on Step 2 and allow retry.
- No backward navigation through steps.
- The only alternative exit is factory reset (long-press profile photo).
- iPhone pairing is optional; user may skip and pair later via long-press.

## Factory Reset

- **Trigger**: long-press the circular profile photo for 3 seconds from any state.
- Shows a confirmation popup with Cancel and Reset actions.
- Erases: profile, meetings, PTO, and pairing bonds.
- Device returns to first-boot setup state.

## Runtime Re-Pair iPhone

- **Trigger**: long-press the phone-disconnect icon in the status bar for 3 seconds.
- Available from every runtime state (not during first-boot setup).
- Status bar **hidden** during re-pair — layout identical to Step 3.
- A Cancel button returns to the main screen.
