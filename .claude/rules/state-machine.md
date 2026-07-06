# Ori — Left Panel State Machine

The left panel has **two user-selectable modes** via the status-bar mode-toggle button: **Calendar** (default) and **Controls** (requires Orion). Toggle is visible when Orion is connected; hidden when offline.

**Clock** is a third view entered by tapping the date/time in the status bar — not part of the toggle cycle. While in Clock, the mode-toggle becomes a **return** button (calendar icon, neutral style) that restores the previous mode.

**Calendar (month view)** is a fourth view entered by long-pressing the date/time in the status bar — also not part of the toggle cycle, and view-only (no meeting data overlaid). It shares Clock's exit mechanics: the mode-toggle becomes the same return button and restores the previous mode.

## Priority Order (Highest → Lowest)

1. **OTA-Updating** — firmware update in progress. Full-screen takeover; status bar and profile card hidden; touch inert. See `ota.md`.
2. **Time Off active** — current local time falls within the cached Time Off window.
3. **5-minute pre-meeting countdown modal** — exactly 5 minutes before any meeting start.
4. **Reconnect-Syncing overlay** — Orion reconnected and running the hash-manifest sync. Overlays left panel only; status bar and profile card remain visible.
5. **Mode-driven content** (when no higher-priority state is active):
   - **Calendar mode** — meeting list or "No meetings today." See `meeting-list.md`.
   - **Clock view** — digital clock, entered via status-bar time tap; status bar date/time hidden.
   - **Calendar view** — month grid, entered via status-bar time long-press; status bar date/time hidden.
   - **Controls mode** — media-controller UI. Only available when Orion is connected; auto-reverts to Calendar on PC disconnect. See `media-mode.md`.

The right panel and status bar remain visible in all states **except OTA-Updating**.

## State Descriptions

### Time Off Active
Left panel: Time Off destination scenic image fills the full panel. A frosted-dark card anchored to the bottom overlays the image with the "On Time Off" eyebrow label, destination name, and date range (guarantees readability against any image colour). When Time Off ends while offline: show "No meetings today."

### 5-Minute Countdown Modal
- Trigger: exactly 5 minutes before a meeting's start time.
- Display: centred circular countdown ring overlaying all other content.
- Dismiss: **Close** button. Once dismissed, does not reappear until reboot.
- If device reconnects after the 5-minute window has already started, show the alert immediately.

### Clock
- **Entry:** tap the date/time text in the status bar.
- **Exit:** tap the mode-toggle button → returns to the mode active before entering Clock.
- Status bar date/time hidden; mode-toggle always visible (even when Orion is offline).
- Countdown modal still fires on top of Clock when triggered.
- High-priority states (OTA, Time Off) override Clock; meeting list updates do not exit Clock.
- **Two faces, one state:** Digital (`screen_clock.cpp`, ~96 px digits-only font) and Analog (`screen_clock_analog.cpp`, 280 px tick dial + hands, accent second hand). `build_clock_screen()` (`state_machine.cpp`) picks between them via `g_clock_face` (0=Digital, 1=Analog), persisted through `nvs::get/set_clock_face()` — survives power cycles, same as the calendar/media mode preference. Both faces apply the same `clock_is_set()` fallback (no battery-backed RTC — see `meeting-list.md`): digital shows "--:--"/"WAITING FOR ORION", analog parks all hands at 12 with the same waiting label. Settable over BLE via the **Device Settings** characteristic (`ble-protocol.md` §3/§6.4, char `000E`, key `"c"`) — Orion writes it as part of the Device Settings CBOR map, applied + persisted immediately (not staged through §6.0's BEGIN/END pipeline). Also reachable via the `ORI_DEBUG_SERIAL` cycler (`c` = Digital, `a` = Analog) for hardware testing without a real Orion build (PC_app/M6 not started yet).

### Calendar (Month View)
- **Entry:** long-press (1 s) the date/time text in the status bar — a custom, shorter-than-default hold timed off PRESSED/RELEASED on that widget only; the shared indev long-press threshold (3 s, set in `main.cpp`) still governs the profile-photo and phone-icon long-presses.
- **Exit:** tap the mode-toggle button → returns to the mode active before entering Calendar (same `g_pre_clock_mode` restore as Clock).
- Status bar date/time hidden; mode-toggle always visible (even when Orion is offline).
- View-only: a 7-column month grid (weekday header + day cells), today highlighted in an accent-filled circle, with up/down chevrons in the header to navigate between months. No meeting data is overlaid — the meeting list is RAM-only (`meeting-list.md`), so there is nothing reliable to show beyond today even if it were.
- Navigating months only re-renders the grid in place; it does not leave or re-enter the Calendar state.
- Countdown modal still fires on top of Calendar when triggered.
- High-priority states (OTA, Time Off) override Calendar; meeting list updates do not exit Calendar.
- Re-entering Calendar (after exiting and long-pressing again) always resets the view back to the current month.

### Reconnect-Syncing Overlay
- Trigger: a real `SyncControl{op:"BEGIN"}` frame, NOT the underlying BLE
  connection — Orion's background service might not even be running yet when
  the BLE link comes up, so the overlay waits for actual proof a sync is
  starting (see `ble-protocol.md` §6.2). Only shown when the BEGIN's declared
  `total` exceeds `RECONNECT_OVERLAY_MIN_BYTES` (200 B, `ble_manager.cpp`) —
  Time Sync + Shortcut Config alone, sent unconditionally on every periodic
  refresh (`ble-protocol.md` §6.3), total well under that and were
  deliberately built to be invisible (no blackout, no rebuild); any sync that
  also carries Profile/Photo/Meetings/Time Off is comfortably larger.
- Display: circular progress ring overlaying the left panel. Copy: **"Reconnecting to Orion…"** / **"Refreshing your day"**. The ring is driven by real byte progress as data is written into the PSRAM staging buffers — same `received/total` mechanism and `OrioningProgress` event as the Step 2/3 Orioning ring (`ble-protocol.md` §6.0). Capped at 99% until the sync commits at `END`, then jumps to 100% just before the overlay dismisses.
- **Shown on every qualifying resync** — including the very first sync after a
  fresh boot, when the meeting list is still empty (meetings are RAM-only).
  Profile, Photo, and Time Off are NVS-backed and can be large enough that the sync
  takes a while and ends in a display blackout for the flash commit
  (`ble-protocol.md` §6.0); without the overlay the user would otherwise sit
  on a static "No meetings today" and then have the screen go black with no
  explanation. `on_reconnect_end()` re-evaluates to the real meeting list (or
  back to "No meetings today") once the sync finishes.
- Auto-dismisses when Device Status returns to `RUNTIME_READY` (typically <500 ms when nothing changed).
- Not user-dismissable; touch on overlay is inert.
- Does **not** appear for periodic in-session refreshes.

### OTA-Updating Screen
- Trigger: firmware update accepted over USB CDC. State is local to firmware — not signalled over BLE.
- Full-screen takeover: status bar, profile card, and left panel all hidden.
- Content: "Updating firmware… N%" with a progress indicator.
- All touch inert; non-dismissable until update validates and reboots, or fails and returns to runtime.
