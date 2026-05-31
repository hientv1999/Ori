# Ori — Left Panel State Machine

The left panel has **two user-selectable modes** cycled by the status-bar mode-toggle button: **Calendar** (default) and **Controls** (requires Orion). The mode-toggle is always visible when Orion is connected; hidden when offline.

**Clock** is a third view entered separately by **tapping the date/time in the status bar** — it is not part of the mode-toggle cycle. While in Clock, the mode-toggle button becomes a **return** button (calendar icon, neutral style) that goes back to whichever mode was active before the time tap.

High-priority system states (OTA, PTO, countdown, reconnect) override the left panel. The 5-minute pre-meeting countdown modal overlays whichever mode is active when it fires.

## Priority Order (Highest → Lowest)

1. **OTA-Updating** — firmware update in progress (USB CDC). Full-screen takeover; status bar and profile card hidden; touch inert. See `ota.md`.
2. **PTO active** — current local time falls within the cached PTO window.
3. **5-minute pre-meeting countdown modal** — exactly 5 minutes before any meeting start.
4. **Reconnect-Syncing overlay** — Orion reconnected and is running the hash-manifest sync. Overlays the left panel only; status bar and profile card remain visible. See §Reconnect-Syncing below.
5. **Mode-driven left panel content** (when no higher-priority state is active):
   - **Calendar mode** — meeting list (or "No meetings today" when empty). No work-hours restriction; the meeting list is always the base Calendar view.
   - **Clock view** — digital clock, entered by tapping the status-bar time (not via the mode-toggle cycle); status bar date/time hidden. See §Clock below.
   - **Controls mode** — media-controller UI (see `keyboard-mode.md`). Only available when Orion is connected; auto-reverts to Calendar on PC disconnect.

Higher-priority states override lower ones. The right panel (profile card) and status bar remain visible in all states **except OTA-Updating**.

## State Descriptions

### PTO Active
- Left panel: PTO destination scenic image fills the full panel. A frosted-dark floating card (semi-transparent dark background + blur) is anchored to the bottom of the panel, overlaying the image, and contains the "On PTO" eyebrow label, destination name, and date range. The card guarantees text readability against any image colour.
- Profile card remains on the right.
- Status bar remains active.
- The 5-minute countdown does **not** appear during an active PTO period.
- When PTO ends while offline: show "No meetings today".

### 5-Minute Countdown Modal
- Trigger: exactly 5 minutes before a meeting's start time.
- Display: centered circular countdown ring overlaying all other content.
- Dismiss: tap the **Close** button on the modal.
- Once dismissed, the popup does not reappear until reboot.
- If the device reconnects after the 5-minute window has already started, show the alert immediately when the meeting is detected.
- Not shown during active PTO.

### Meeting List / No Meetings (Calendar Mode)
- Always the Calendar mode view — no work-hours restriction.
- If the meeting list is empty: show "No meetings today".
- See `meeting-list.md` for full meeting list rules.

### Clock
- **Entry:** user taps the date/time text in the status bar.
- **Exit:** user taps the mode-toggle button (calendar icon, neutral style) → returns to the mode that was active before entering Clock (Calendar or Controls).
- Status bar date/time is **hidden** (the clock itself provides the time). The mode-toggle button is always visible in this state, even when Orion is offline.
- Profile card remains visible.
- The countdown modal still fires on top of the clock when a meeting is 5 minutes away.
- High-priority states (OTA-Updating, PTO active) override Clock; normal Calendar state changes (meeting list updates) do not exit Clock.

### Reconnect-Syncing Overlay
- Trigger: BLE link to Orion re-established and Device Status transitions to `RUNTIME_RECONNECTING` (see `ble-protocol.md` §6.2).
- Display: a circular progress ring overlays the left panel, reusing the Step 3 "Orioning" visual component. Copy: **"Reconnecting to Orion…"** with subtitle **"Refreshing your day"**.
- Status bar and profile card remain visible — only the left panel is masked.
- Auto-dismisses the moment Device Status returns to `RUNTIME_READY`. When nothing changed, this is typically <500 ms.
- Not user-dismissable. Touch on the overlay is inert.
- Does **not** appear for periodic in-session refreshes (those run in the background).

### OTA-Updating Screen
- Trigger: a firmware update has been accepted over USB CDC (Orion-driven). See `ota.md`. The state is local to the firmware — it is **not** signalled over BLE in v1.1.
- Full-screen takeover: status bar hidden, profile card hidden, left panel hidden.
- Content: "Updating firmware… N%" with a progress indicator.
- All touch is inert. The two-finger backlight gesture and 3-second long-press triggers do nothing.
- Non-dismissable until the update either validates and reboots, or fails and returns to normal runtime. (The only physical escape is unplugging USB-C — safe, since the inactive slot is what was being written.)
