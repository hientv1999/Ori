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

**The meeting list is RAM-only — it is NOT persisted to flash.** A power cycle
clears it; the list is rebuilt only when Orion reconnects and re-pushes it.

| Condition | Left panel (Calendar mode) |
|---|---|
| Orion connected & synced | Live meeting list |
| Orion disconnected at runtime (meetings still in RAM) | Cached meeting list + "SYNCED · X min ago" pill |
| After a power cycle (RAM cleared), before re-sync | "No meetings today" |
| Within Time Off window | Time Off destination visual (overrides Calendar) |

**Why RAM-only:** local time is also not restored from flash on a cold boot
(there is no battery-backed RTC). Without a valid clock the time-based logic
(5-minute alert, in-progress red, expiry) can't run, so showing a persisted-but-
stale meeting list would mislead. Keeping meetings in RAM ties their lifetime to
having a live clock. The "SYNCED · X min ago" pill therefore only appears on a
**runtime** Orion disconnect (meetings + clock still live in RAM), never after a
power cycle. Local time comes from Orion (primary) or the iPhone's Current Time
Service (secondary backup) — see `connectivity.md`; when neither has provided it,
the status-bar clock is hidden entirely (no "--:--").
