# Ori — Meeting List Rules

## Each Meeting Item Shows

- Start time and end time
- Meeting title
- Location
- Organizer

## Title and Location Handling

- **Title: single line, ellipsis on overflow.** Row height is fixed; no wrapping.
- **Location: single line, ellipsis on overflow.** Truncates before the organizer dot-separator so the organizer name stays visible.
- **Tap-to-expand:** tapping a meeting row opens a full-screen detail overlay showing the full title, full location, and start–end time. Dismissed via **Close** button only. Text-only — no countdown ring.

## Sorting and Overlaps

- Overlapping meetings are displayed as separate list items — do not merge.
- Sort order: start time ascending → earlier end time first → original calendar order.
- Two meetings with the same start time are shown as separate rows, both accent-colored to flag the overlap.

## In-Progress Highlight

- A meeting where `start ≤ now ≤ end` is **in-progress**: start time, end time, and title render in `COLOR_DANGER` (red).
- When in-progress and overlapping, in-progress red wins over the overlap accent color.
- The flag is recomputed on the same timer that drives meeting expiry: `now` crosses `start` → row turns red; `now` crosses `end` → row is removed.

## List Lifecycle

- Past meetings are removed immediately once their end time passes.
- Cancelled meetings are never shown.
- The list is vertically scrollable with a visible scrollbar on the right edge.

## Offline / Cached State

| Condition | Left panel (Calendar mode) |
|---|---|
| Orion synced | Live meeting list |
| BLE only (not synced) | Cached meeting list + "SYNCED · X min ago" pill |
| Fully offline + cache exists | Cached meeting list + "SYNCED · X min ago" pill |
| Fully offline + no cached data | "No meetings today" |
| Within PTO window | PTO destination visual (overrides Calendar) |

Local time is set by Orion on sync, persists in flash, and drives all time-based logic while offline.
