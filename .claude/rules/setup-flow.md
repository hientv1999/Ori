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

### iPhone bond — forced reconnect (first-boot only)

iOS doesn't reliably flush the ANCS notification backlog (the "PreExisting" replay of already-existing notifications) on the **same connection** where the bond was just created — only on connections after that. So after the Step 3 iPhone bond completes and ANCS subscribes, Ori deliberately drops just the iPhone link and lets it auto-reconnect (existing bonded-disconnect → re-advertise → iOS auto-reconnect machinery). That reconnect is what actually triggers iOS to deliver the notification backlog.

The drop is deferred until **after** the Setup Complete screen's 5 s timer hands off to the runtime screen (`ble_manager::run_pending_ancs_backlog_reconnect()`, called from `state_machine::poll()`) rather than fired on a short timer right after bonding. ANCS backlog processing — per-notification parsing plus icon-registry lookups across up to 48 apps — is comparatively heavy, and running it while Setup Complete's checkmark-ring/countdown-bar animations are live would compete with LVGL for the same core; the runtime screen it hands off to has no comparable animation to protect. Trade-off: unlike the old during-linger timing, the status bar is now visible on the runtime screen, so the phone icon may briefly show disconnected/reconnecting there.

This is gated to **first-boot setup only** (`nvs::is_first_boot()`, captured *before* the screen transition to Setup Complete — that transition flips the flag) — a runtime re-pair (below) does not force a reconnect.

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

- 6-digit BLE passkey displayed in a modal overlay on top of the Link Orion base screen — Ori is the display side of LE Secure Connections Passkey Entry.
- User confirms the displayed code in the PC's own OS Bluetooth pairing prompt (Orion is the entry side, not a mutual-display confirm — this is OS-native on the PC, not a custom Orion UI, see `pc-app.md`/`memory.md`).
- Dismissed automatically when bonding completes; the Orioning modal then appears on the same base screen.

### Setup failure rules

- If PC pairing fails, stay on Step 2 and allow retry.
- No backward navigation through steps.
- The only alternative exit is factory reset (long-press profile photo).
- iPhone pairing is optional; user may skip and pair later by tapping the status-bar phone icon.

## Factory Reset

- **Trigger**: long-press the circular profile photo for 3 seconds from any state.
- Shows a confirmation popup with Cancel and Reset actions.
- Erases: profile, meetings, Time Off, and pairing bonds.
- Device returns to first-boot setup state.

## Runtime Re-Pair iPhone

- **Trigger**: tap the status-bar phone icon while the iPhone is disconnected (the icon is always visible; red = disconnected). If a stale bond exists it is wiped automatically so the iPhone slot reopens.
- Tapping the phone icon while **connected** opens the **Unpair iPhone** modal instead (personalised with the phone's GAP device name).
- Available from every runtime state (not during first-boot setup).
- Status bar **hidden** during re-pair — layout identical to Step 3.
- A Cancel button returns to the main screen.
- **No forced reconnect here** — the first-boot-only ANCS-backlog workaround above doesn't apply; a runtime re-pair bond is left alone after subscribing.
