# Ori — Product Intent

Ori is a desk-based status and awareness display. It is **not** a task manager, second phone screen, or personal dashboard.

## Goals

- Show availability and meetings clearly
- Provide non-intrusive notification awareness — icons in the status bar, full content accessible via tap (title + body, read-only)
- Work reliably even when fully offline
- Be mass-producible and transferable between users via factory reset

## Non-Goals (Explicitly Out of Scope)

- No task lists
- No composing or sending replies to message notifications — ANCS content
  (title/body) is view-only. The one exception is call control: incoming
  calls can be Answered/Declined and active calls Ended, since ANCS exposes
  these as a binary accept/reject action on the notification itself, not
  authored content — see `connectivity.md`.
- No complex personal dashboards

## Design Philosophy

- Clarity over density
- All state changes must feel predictable and calm
