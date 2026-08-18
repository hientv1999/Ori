# Ori — Left Panel State Machine

The left panel has **two user-selectable modes** via the status-bar mode-toggle button: **Calendar** (default) and **Controls** (requires Orion). Toggle is visible when Orion is connected; hidden when offline.

**Clock** is a third view entered by tapping the date/time in the status bar — not part of the toggle cycle. While in Clock, the mode-toggle becomes a **return** button (calendar icon, neutral style) that restores the previous mode.

**Calendar (month view)** is a fourth view entered by long-pressing the date/time in the status bar — also not part of the toggle cycle, and carries no meeting data (a day cell showing a local holiday is tappable — `gestures.md` — but nothing here reflects the meeting list). It shares Clock's exit mechanics: the mode-toggle becomes the same return button and restores the previous mode.

## Priority Order (Highest → Lowest)

1. **OTA-Updating** — firmware update in progress. Full-screen takeover; status bar and profile card hidden; touch inert. See `ota.md`.
2. **Time Off active** — current local time falls within the cached Time Off window.
3. **5-minute pre-meeting countdown modal** — exactly 5 minutes before any meeting start.
4. **Reconnect-Syncing overlay** — Orion reconnected and running the hash-manifest sync. Overlays left panel only; status bar and profile card remain visible.
5. **Mode-driven content** (when no higher-priority state is active):
   - **Calendar mode** — meeting list or "No meetings today." See `meeting-list.md`.
   - **Clock view** — digital clock, entered via status-bar time tap; status bar date/time hidden.
   - **Calendar view** — month grid, entered via status-bar time long-press; status bar date/time hidden.
   - *Persistence exception:* Clock and Calendar view are entered by an explicit user action, so once open they are **held above** the passive higher-priority states (Time Off, Reconnect-Syncing) until the user exits via the mode-toggle return button. Only a full-screen takeover (OTA, Setup) pulls the user out. The 5-minute countdown still appears — but as a modal *overlay on top of* Clock/Calendar, not by replacing them.
   - **Controls mode** — media-controller UI. Only available once Orion has fully synced (not merely connected — the mode-toggle that's the only way in stays hidden through a reconnect's resync window); auto-reverts to Calendar on PC disconnect. See `media-mode.md`.

The right panel and status bar remain visible in all states **except OTA-Updating**.

## State Descriptions

### Time Off Active
Left panel: Time Off destination scenic image fills the full panel. A frosted-dark card anchored to the bottom overlays the image with the "On Time Off" eyebrow label, destination name, and date range (guarantees readability against any image colour). When Time Off ends while offline: show "No meetings today."

### 5-Minute Countdown Modal
- Trigger: exactly 5 minutes before a meeting's start time (automatic), or the user tapping a meeting row in the list that's already inside that window (`meeting-list.md` "Imminent-meeting exception" — takes over from the regular detail overlay for that tap).
- Display: centred circular countdown ring overlaying all other content.
- Dismiss: **Close** button. Once dismissed, does not reappear until reboot — tapping the meeting row again also marks it alerted, so it doesn't reappear automatically either.
- If device reconnects after the 5-minute window has already started, show the alert immediately.

### Clock
- **Entry:** tap the date/time text in the status bar.
- **Exit:** tap the mode-toggle button → returns to the mode active before entering Clock.
- Status bar date/time hidden; mode-toggle always visible (even when Orion is offline).
- Countdown modal still fires on top of Clock when triggered.
- Only a full-screen takeover (OTA, Setup) overrides Clock. Passive state changes do **not** exit Clock — neither meeting-list updates nor Time Off becoming (or staying) active while the user is in Clock. Exit is always via the mode-toggle return button, which drops back to the mode active before Clock (so a still-active Time Off window reappears then).
- **Two faces, one state:** Digital (`screen_clock.cpp`, ~96 px digits-only font) and Analog (`screen_clock_analog.cpp`, 280 px tick dial + hands, accent second hand). `build_clock_screen()` (`state_machine.cpp`) picks between them via `g_clock_face` (0=Digital, 1=Analog), persisted through `nvs::get/set_clock_face()` — survives power cycles, same as the calendar/media mode preference. Both faces apply the same `clock_is_set()` fallback (no battery-backed RTC — see `meeting-list.md`): digital shows "--:--"/"WAITING FOR ORION", analog parks all hands at 12 with the same waiting label. Settable over BLE via the **Device Settings** characteristic (`ble-protocol.md` §3/§6.4, char `000E`, key `"c"`) — Orion writes it as part of the Device Settings CBOR map, applied + persisted immediately (not staged through §6.0's BEGIN/END pipeline). Also reachable via the `ORI_DEBUG_SERIAL` cycler (`c` = Digital, `a` = Analog) for hardware testing without needing a live Orion sync session.
- **12-/24-hour time format:** independent of the face choice, both faces honour the user's time-format preference (`g_fmt` in `time_format.cpp`, `nvs::get/set_time_format()`, default 24-hour). 24-hour shows "14:30". In 12-hour, since the XL digit-only clock font has no letters, Digital renders "AM"/"PM" as small subtext via a 3-slot row `[left_spacer][center_wrap: hour+colon+minute][right_slot: AM/PM]` where both spacers have `flex_grow=1` and are always present (even empty) — this keeps `center_wrap` centered regardless of `right_slot`'s content (two earlier approaches — manual per-tick realign, and a plain flex sibling — either didn't render on hardware or visibly shifted hour:minute's position). The date strip has no AM/PM suffix in either mode. Analog never shows AM/PM (no digital readout to pair it with) — both faces' date strips read `"WEDNESDAY, MAY 14, 2026"`. Settable over BLE via **Device Settings** (`ble-protocol.md` §3/§6.4, char `000E`, key `"h"`, 0=24-hour 1=12-hour), applied + persisted immediately, and via the `ORI_DEBUG_SERIAL` cycler (`H` toggles). Same preference drives the status-bar clock, meeting-list times, and ANCS notification timestamps.

### Calendar (Month View)
- **Entry:** long-press (1 s) the date/time text in the status bar — a custom, shorter-than-default hold timed off PRESSED/RELEASED on that widget only; the shared indev long-press threshold (3 s, set in `main.cpp`) still governs the profile-photo and phone-icon long-presses.
- **Exit:** tap the mode-toggle button → returns to the mode active before entering Calendar (same `g_pre_clock_mode` restore as Clock).
- Status bar date/time hidden; mode-toggle always visible (even when Orion is offline).
- A 7-column month grid (weekday header + day cells), today highlighted in an accent-filled circle, with up/down chevrons in the header to navigate between months. No meeting data is overlaid — the meeting list is RAM-only (`meeting-list.md`), so there is nothing reliable to show beyond today even if it were.
- **Local holidays** are computed entirely on-device (`holiday_data`, no BLE dependency for the 8 supported countries' — US/VN/CA/GB/AU/ES/MX/FR — national AND well-documented regional rules (fixed-date/nth-weekday/last-weekday/weekday-before/weekday-on-or-after/Easter-offset — e.g. Canadian provinces' own Family Day variants, Australian states' own Labour Day dates); the one exception, Vietnamese Tet, comes from a small lunar-date table Orion pushes once and Ori caches in NVS — `ble-protocol.md` §4/§6.4). A holiday day cell renders with a red ring (composes independently with today's own accent fill/ring — both apply their own outline, so a holiday that's also today shows both) and red day-number text, and is tappable: tapping opens a full-screen detail overlay with the holiday's name, full date, and a short description, dismissed via **Close** button only (same "Close-only, no tap-outside" convention as the meeting detail overlay, `meeting-list.md`) — see `gestures.md`. Non-holiday cells are inert, same as before.
- Navigating months only re-renders the grid in place; it does not leave or re-enter the Calendar state.
- Countdown modal still fires on top of Calendar when triggered.
- Only a full-screen takeover (OTA, Setup) overrides Calendar. Passive state changes do **not** exit Calendar — neither meeting-list updates nor Time Off becoming (or staying) active while the user is in Calendar. Exit is always via the mode-toggle return button, which drops back to the mode active before Calendar (so a still-active Time Off window reappears then).
- Re-entering Calendar (after exiting and long-pressing again) always resets the view back to the current month.

### Weather Alert / Low Battery Alert overlays
- Two dismiss-only "notice" overlays (accent-tinted, not the danger-red factory-reset styling) — end-of-day rain/snow/thunderstorm reminder and bonded-phone low-battery warning. Mirror Orion's own identically-named in-app alerts (`pc-app.md`'s `reminders.rs`/`checkLowBattery()`), now also shown on Ori.
- **Not part of the priority list above** — same treatment as the ANCS notification detail overlay and the incoming-call view (`connectivity.md`): a one-shot overlay built directly on top of whatever's currently on screen, with no `AppState` entry of its own, rather than a state the priority order arbitrates between.
- **Ori arms both independently**, not from an explicit "show this now" push — it already has everything each trigger needs (current weather condition; the bonded phone's live battery %, read directly from the phone's own Battery Service) plus a handful of small config values Orion pushes over Device Settings (work-hours end time + days, each alert's enable flag, offset/threshold — `ble-protocol.md` §4/§6.4). A periodic on-device check (mirroring Orion's own ~60 s tick) evaluates both conditions and fires the overlay itself; Orion's own local copy of each reminder keeps firing independently on the same underlying data.
- Weather Alert fires once per calendar day, `weather_alert_offset_min` before `work_hours_end_min`, on a configured work day, while the current condition is Rain/Thunderstorm/Snow. Low Battery Alert fires once per dip below `low_battery_threshold_pct`, latching until the level recovers or the phone reconnects — same semantics as Orion's own `checkLowBattery()`.
- Dismissed via a single **OK** button only, same "Close-only, no tap-outside" convention as every other Ori detail overlay.

### Reconnect-Syncing Overlay
- Trigger: a real `SyncControl{op:"BEGIN"}` frame, NOT the underlying BLE connection — Orion's background service might not even be running yet when the BLE link comes up, so the overlay waits for actual proof a sync is starting (`ble-protocol.md` §6.2). Only shown when the BEGIN's declared `total` exceeds `RECONNECT_OVERLAY_MIN_BYTES` (200 B, `ble_manager.cpp`) — Time Sync + Shortcut Config alone (sent unconditionally every periodic refresh, §6.3) are well under that and deliberately invisible; any sync carrying Profile/Photo/Meetings/Time Off is comfortably larger.
- Display: circular progress ring overlaying the left panel. Copy: **"Reconnecting to Orion…"** / **"Refreshing your day"**. Driven by real byte progress into the PSRAM staging buffers — same `received/total` mechanism as the Step 2/3 Orioning ring (§6.0). Capped at 99% until `END` commits, then jumps to 100% just before dismissing.
- **Shown on every qualifying resync** — including the very first sync after a fresh boot, when the meeting list is still empty (RAM-only). Profile/Photo/Time Off are NVS-backed and can end in a display blackout for the flash commit (§6.0); without the overlay the user would sit on "No meetings today" then have the screen go black unexplained. `on_reconnect_end()` re-evaluates to the real meeting list (or back to "No meetings today") once the sync finishes.
- Auto-dismisses when Device Status returns to `RUNTIME_READY` (typically <500 ms when nothing changed).
- Not user-dismissable; touch on overlay is inert.
- Does **not** appear for periodic in-session refreshes.

### OTA-Updating Screen
- Trigger: firmware update accepted over BLE (`ble-protocol.md` §14). The state itself is still local to firmware — deliberately NOT signalled through Device Status (`0x20` stays reserved and unused); Orion already knows, since it is the one sending the image.
- Full-screen takeover: status bar, profile card, and left panel all hidden.
- Content: "Updating firmware… N%" with a progress indicator.
- All touch inert; non-dismissable until update validates and reboots, or fails and returns to runtime.
