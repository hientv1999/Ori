# Ori — Meeting List Rules

## Each Meeting Item Shows

- Start time and end time
- Meeting title
- Location
- Organizer

## Title and Location Handling

- **Title: single line, ellipsis on overflow.** Row height is fixed; no wrapping.
- **Location: single line, ellipsis on overflow.** Location truncates before the organizer dot-separator so the organizer name stays visible.
- **Tap-to-expand:** tapping a meeting row opens a full-screen detail overlay (same scrim style as the 5-minute countdown modal) showing the **full** title, full location, and start–end time. Dismissed via the **Close** button only. The countdown ring is absent — this overlay is text-only.
- Row height is uniform across all meetings (no variable height due to wrapping). This makes the list compact and allows more meetings to be visible without scrolling.

## Sorting and Overlaps

- Overlapping meetings are displayed as separate list items — do not merge.
- Sort order: start time ascending → earlier end time first → original calendar order.
- Two meetings with the same start time are shown as separate rows, both accent-colored to flag the overlap.

## In-Progress Highlight

- A meeting whose window contains the current local time (`start ≤ now ≤ end`) is treated as **in-progress**.
- The row's start time, end time, and title are rendered in `COLOR_DANGER` (the same red used elsewhere on the device — e.g. the phone-disconnect icon) so the user can spot the meeting they should be attending right now at a glance.
- When a meeting is simultaneously in-progress and overlapping with another, the in-progress red wins — "you should be in this room right now" is more actionable than "this overlaps with another item".
- The in-progress flag is recomputed by the firmware on the same timer that drives meeting expiry (M4): when `now` crosses a meeting's `start`, that row gains the red highlight; when `now` crosses its `end`, the row is removed entirely per the lifecycle rule below.

## List Lifecycle

- Past meetings are removed immediately once their end time passes.
- Cancelled meetings are never shown.
- The list is vertically scrollable with a visible scrollbar on the right edge to indicate total length and current scroll position.

## Offline / Cached State

| Condition | Left panel |
|---|---|
| Work hours + Orion synced | Live meeting list |
| Work hours + BLE only (not synced) | Cached meeting list + "SYNCED · X min ago" pill |
| Work hours + fully offline + cache exists | Cached meeting list + "SYNCED · X min ago" pill |
| Work hours + no cached data | "No meetings today" |
| After hours | Digital clock |
| Within PTO window | PTO destination visual |

Local time is set by Orion on sync. It persists in flash and drives all time-based logic while offline.
