# Ori — Setup Flow and Factory Reset

## First-Time Setup

Shown on first boot or after a factory reset. The status bar is **hidden** for the entire flow.

A **3-dot** progress indicator is anchored at a fixed Y position near the bottom of every setup screen (Welcome through Step 3). The dot row must not move between pages.

### Screen sequence

| Screen | Dot state | Notes |
|---|---|---|
| Welcome | All 3 dots inactive | Ori wordmark; "Start" button advances |
| Step 1 — Install Orion | Dot 0 active | Visit `orinari.net/orion` on PC; user taps Next after install |
| Step 2 — Link Orion | Dot 1 active | Base: BLE name + spinner. **Passkey modal** overlays when Orion connects. **Orioning modal** overlays during sync (progress ring, "A busy day ahead…"). Both modals dismissed automatically by state transitions — not by the user. |
| Step 3 — iPhone/iPad pairing | Dot 2 active | Optional; user may skip; pairing can be done later via long-press. Screen copy reads "Connect on iPhone or iPad" since the device isn't bonded yet — see `connectivity.md` §2 |
| Setup complete | Dots **hidden** | Brief acknowledgement before transitioning to normal runtime |

### iPhone/iPad bond — forced reconnect (first-boot AND runtime re-pair)

iOS/iPadOS doesn't reliably flush the ANCS notification backlog (the "PreExisting" replay) on the **same connection** where the bond was just created — only on connections after that. An iOS-side quirk of the fresh bond itself, not first-boot-specific — a runtime re-pair without this fix never loads ANCS icons either. So after **any** iPhone/iPad bond completes and ANCS subscribes (Step 3 first-boot, or a later runtime re-pair), Ori deliberately drops the phone link and lets it auto-reconnect (bonded-disconnect → re-advertise → iOS auto-reconnect) — that reconnect is what triggers iOS to deliver the backlog.

Timing differs by path (`ble_manager::run_pending_ancs_backlog_reconnect()`):

- **First-boot setup**: deferred until **after** the Setup Complete screen's 5 s timer hands off to the runtime screen, rather than fired right after bonding — ANCS backlog processing (per-notification parsing + icon lookups across up to 48 apps) is heavy enough to compete with LVGL for the same core while Setup Complete's animations are live; the runtime screen has no comparable animation to protect. Trade-off: the status bar is visible on the runtime screen, so the phone icon may briefly show disconnected/reconnecting there. Gated on `nvs::is_first_boot()`, captured before the transition to Setup Complete (which flips the flag).
- **Runtime re-pair**: fired immediately once bonding completes — dismissing the re-pair screen returns straight to whatever screen launched it, no animation-heavy hand-off to protect.

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
| Step dots | Fixed at `DOT_ROW_Y = 456` px |

### Passkey modal (Step 2)

- 6-digit BLE passkey displayed in a modal overlay on top of the Link Orion base screen — Ori is the display side of LE Secure Connections Passkey Entry.
- User types the displayed code into Orion's own custom passkey modal on the PC — six digit boxes with auto-advance focus (Orion is the entry side, not a mutual-display confirm; see `pc-app.md`/`memory.md`). Windows only for now — macOS has no equivalent app-level pairing hook and is deferred until that build starts.
- Dismissed automatically when bonding completes; the Orioning modal then appears on the same base screen.

### Setup failure rules

- If PC pairing fails, stay on Step 2 and allow retry.
- No backward navigation through steps.
- The only alternative exit is factory reset (long-press profile photo).
- iPhone/iPad pairing is optional; user may skip and pair later by tapping the status-bar phone icon.

## Factory Reset

- **Trigger**: long-press the circular profile photo for 3 seconds from any state.
- Shows a confirmation popup with Cancel and Reset actions.
- Erases: profile, meetings, Time Off, and pairing bonds.
- Device returns to first-boot setup state.

## Runtime Re-Pair iPhone/iPad

- **Trigger**: tap the status-bar phone icon while the phone is disconnected (the icon is always visible; red = disconnected). If a stale bond exists it is wiped automatically so the slot reopens.
- Tapping the phone icon while **connected** opens the **Unpair** modal instead (personalised with the phone's GAP device name; title reads "Unpair iPhone"/"Unpair iPad" once the Model Number String is known, else the generic "Unpair iPhone or iPad" — `connectivity.md` §2).
- Available from every runtime state (not during first-boot setup).
- Status bar **hidden** during re-pair — layout identical to Step 3.
- A Cancel button returns to the main screen.
- **Forced reconnect applies here too** — see "iPhone/iPad bond — forced reconnect" above. Once bonding completes, Ori immediately drops and lets the phone auto-reconnect so ANCS delivers its notification backlog; the phone icon may briefly show disconnected/reconnecting on the screen the re-pair returned to.
