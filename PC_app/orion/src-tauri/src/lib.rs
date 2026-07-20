mod ble;
mod calendar_import;
mod commands;
mod holiday;
mod notif_filter;
mod reminders;
mod store;
mod weather;

use tauri::{
    image::Image,
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    AppHandle, Manager, WebviewWindow,
};

// Looked back up via `app.tray_by_id` whenever the connection state changes,
// so `set_tray_status` can re-badge the existing icon instead of rebuilding
// the tray entry.
const TRAY_ID: &str = "orion-tray";

// Windows has no OS-level automatic window-corner rounding (that's an 11-only
// DWM feature) and WebView2's CSS `border-radius` + transparent-window alpha
// doesn't reliably reach the native window surface — measured on-device as
// zero transparency at the geometric corner despite the CSS being correct.
// Clipping the actual HWND region is the only reliable fix on Windows 10+.
//
// GDI's CreateRoundRectRgn cuts a hard, non-anti-aliased edge; the browser's
// own `border-radius` curve is smooth/anti-aliased. Where the two don't
// exactly coincide, WebView2 has to paint *something* for the sliver of
// window that's inside the OS region but outside the CSS curve — and since
// it can't do true per-pixel transparency here either, that sliver showed up
// as a faint phantom dark seam tracing the corner. Fix: don't inset the flat
// edges (that would clip the panel's own 1px border stroke on the straight
// sides), but make the *radius* used for the OS clip a couple pixels larger
// than the CSS radius — a bigger radius cuts away more at the corner, so the
// hard-edged region ends up a subset of the smooth curve (tucked just inside
// it) instead of poking past it.
// LOGICAL_WIDTH/HEIGHT mirror tauri.conf.json's fixed window size
// (resizable:false, 348x810) — kept in sync manually since this file has no
// access to that JSON at compile time.
#[cfg(target_os = "windows")]
const LOGICAL_WIDTH: f64 = 348.0;
#[cfg(target_os = "windows")]
const LOGICAL_HEIGHT: f64 = 810.0;

#[cfg(target_os = "windows")]
fn apply_rounded_corners(window: &WebviewWindow) {
    use windows::Win32::Graphics::Gdi::CreateRoundRectRgn;

    let Ok(hwnd) = window.hwnd() else { return };
    // `WebviewWindow::hwnd()` returns *tauri's own* `windows` crate's `HWND`
    // (tauri/wry/tao still pin `windows` 0.61, one major version behind our
    // own direct 0.62 dependency — see Cargo.toml) — a different nominal
    // type from our `windows::Win32::Foundation::HWND` even though both are
    // the exact same `#[repr(transparent)] struct HWND(pub *mut c_void)`.
    // Re-wrap the raw pointer in our own crate's `HWND` rather than trying to
    // pass tauri's across the boundary.
    let hwnd = windows::Win32::Foundation::HWND(hwnd.0 as *mut core::ffi::c_void);

    // Deliberately NOT window.outer_size() — a genuine (if not, in the end,
    // the culprit for the 2026-07 release-only click-failure bug — that
    // turned out to be a CSP/inline-handler issue, see tauri.conf.json)
    // robustness fix: tao/winit have a well-documented class of startup
    // races where the physical size queried immediately after window
    // creation is stale or zero (rust-windowing/winit#581/#923/#2094;
    // tauri-apps/tauri#12152 reports outer_size() == (0,0) right after
    // creation). A degenerate CreateRoundRectRgn(0,0,1,1,...) from that
    // would clip SetWindowRgn's mouse hit-testing to a literal single pixel
    // — WebView2 keeps compositing the full frame regardless (GDI region
    // clipping and its own composited surface aren't gated together), so
    // the window would look completely normal while nothing is clickable.
    // The window's logical footprint is fixed and known statically
    // (resizable:false, tauri.conf.json), so derive physical size from that
    // instead of asking the OS for a value that can race.
    let scale = window.scale_factor().unwrap_or(1.0);
    let width = (LOGICAL_WIDTH * scale).round() as i32;
    let height = (LOGICAL_HEIGHT * scale).round() as i32;
    // Sanity floor: if scale_factor() itself is still unsettled (e.g. reads
    // back 0.0 before DPI resolves), skip this pass rather than commit a
    // broken clip region — the Focused/Resized re-apply below will retry
    // once it's settled.
    if width < 100 || height < 100 {
        return;
    }

    let css_radius = 19.5 * scale; // matches --r in styles.css
    let clip_radius = (css_radius + 12.0 * scale).round() as i32;
    let region = unsafe {
        CreateRoundRectRgn(0, 0, width + 1, height + 1, clip_radius, clip_radius)
    };
    unsafe {
        let _ = windows::Win32::Graphics::Gdi::SetWindowRgn(hwnd, Some(region), true);
    }
}

// Shows (and un-minimizes) the panel and gives it focus — shared by the
// tray icon's "open" click branch below (`toggle_panel`) and
// `ble::central`'s incoming-call watcher, which calls `show_and_focus_panel`
// on an `AncsCallState{st:1}` ringing notify (ble-protocol.md §13 "Call
// takeover") to raise the window exactly as clicking the tray icon would,
// even if the panel was closed or minimized when the call came in.
fn focus_panel(window: &WebviewWindow) {
    let _ = window.unminimize();
    let _ = window.show();
    let _ = window.set_focus();
}

// The panel stays visible until the user explicitly minimizes it (the
// in-app minimize button, or clicking the tray icon again) — it does NOT
// auto-hide on focus loss. Toggling preserves wherever the user last moved
// the window (no re-anchoring to the tray icon's click point).
fn toggle_panel(window: &WebviewWindow) {
    if window.is_visible().unwrap_or(false) {
        let _ = window.hide();
    } else {
        focus_panel(window);
    }
}

/// Public entry point for `ble::central`'s call-takeover watcher, which only
/// has an `AppHandle` (not a `WebviewWindow` reference) — resolves the
/// "main" window and delegates to the exact same `focus_panel` routine the
/// tray icon's own click-to-open branch uses, per `ble-protocol.md` §13's
/// "orion-sync owns bringing the window to the foreground."
pub fn show_and_focus_panel(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        focus_panel(&window);
    }
}

// Status-dot badge over the base tray icon — the OS-native counterpart to
// the prototype's fake taskbar mockup (`.tray .tdot`, styles.css), so the
// real system tray shows connected/reconnecting/offline at a glance even
// while the panel is closed. Colors match the in-panel connection dot
// (`--success`/`--away`/`--muted`) so both readouts always agree.
#[derive(Clone, Copy, PartialEq, Eq)]
enum BadgeColor {
    Green,
    Amber,
    Grey,
    Red,
}

impl BadgeColor {
    fn for_state(state: &str) -> Self {
        match state {
            "on" => Self::Green,
            // "connecting" (found Ori, establishing the link) shares the same
            // amber as "rec" (Syncing) — matching styles.css's
            // `.h-dot.rec,.h-dot.connecting`. Without this arm, "connecting"
            // fell into the `_` default below and looked identical to fully
            // disconnected on the tray icon, the primary UI while the panel
            // is closed.
            "rec" | "connecting" => Self::Amber,
            // Bluetooth radio off, or the last calendar/weather refresh
            // failed — app.js's updateTrayStatus() computes this from
            // whichever combination of connState/btAvailable/calendarNetOk/
            // weatherNetOk is current every time any one of them changes
            // (its own doc comment explains why: three independent push
            // sites each reacting only to their own signal let one
            // overwrite another's still-true "error" with a stale "all
            // fine").
            "error" => Self::Red,
            _ => Self::Grey,
        }
    }

    fn rgb(self) -> (u8, u8, u8) {
        match self {
            Self::Green => (0x92, 0xC3, 0x53),
            Self::Amber => (0xFF, 0xAA, 0x44),
            Self::Grey => (0x8A, 0x88, 0x84),
            Self::Red => (0xC4, 0x31, 0x4B), // matches styles.css's --danger
        }
    }
}

// Fills the ring logo's own transparent inner hole with the status color,
// onto a copy of the base icon's raw RGBA — the ring itself (already opaque
// gold in `base`) is left untouched, so this reads as the connection status
// literally glowing through the middle of the mark rather than a separate
// badge competing with it. 0.264 matches the inner-hole radius the source
// icon (icons/icon.png, a ring centered on the square canvas) was drawn
// with — update this ratio if the ring artwork is ever regenerated with
// different proportions.
fn render_badge_icon(base: &Image<'_>, color: BadgeColor) -> Image<'static> {
    let width = base.width();
    let height = base.height();
    let mut rgba = base.rgba().to_vec();
    let (r, g, b) = color.rgb();

    let min_dim = width.min(height) as f32;
    let inner_r = min_dim * 0.264;
    let cx = width as f32 / 2.0;
    let cy = height as f32 / 2.0;

    for y in 0..height {
        for x in 0..width {
            let dx = x as f32 + 0.5 - cx;
            let dy = y as f32 + 0.5 - cy;
            let dist = (dx * dx + dy * dy).sqrt();
            if dist > inner_r {
                continue;
            }
            let idx = ((y * width + x) * 4) as usize;
            rgba[idx] = r;
            rgba[idx + 1] = g;
            rgba[idx + 2] = b;
            rgba[idx + 3] = 255;
        }
    }

    Image::new_owned(rgba, width, height)
}

// The 3 possible badged-icon variants, rendered once at startup — `set_tray_status`
// used to re-run the full per-pixel `render_badge_icon` sweep on every single
// connection-state transition (including transient reconnect flapping), even
// though there are only ever 4 distinct outputs. Cloning a cached `Image` is
// just an owned-buffer copy, far cheaper than recomputing every pixel's
// distance-to-center each time.
struct TrayBadgeIcons {
    green: Image<'static>,
    amber: Image<'static>,
    grey: Image<'static>,
    red: Image<'static>,
}

impl TrayBadgeIcons {
    fn build(base: &Image<'_>) -> Self {
        Self {
            green: render_badge_icon(base, BadgeColor::Green),
            amber: render_badge_icon(base, BadgeColor::Amber),
            grey: render_badge_icon(base, BadgeColor::Grey),
            red: render_badge_icon(base, BadgeColor::Red),
        }
    }

    fn get(&self, color: BadgeColor) -> Image<'static> {
        match color {
            BadgeColor::Green => self.green.clone(),
            BadgeColor::Amber => self.amber.clone(),
            BadgeColor::Grey => self.grey.clone(),
            BadgeColor::Red => self.red.clone(),
        }
    }
}

// Mirrors the panel's own `setConn()` — called from `app.js` on every
// connection-state transition (on/rec/off) so the tray icon never drifts
// from what the panel already shows.
#[tauri::command]
fn set_tray_status(app: AppHandle, state: String) {
    let Some(tray) = app.tray_by_id(TRAY_ID) else { return };
    let Some(icons) = app.try_state::<TrayBadgeIcons>() else { return };
    let _ = tray.set_icon(Some(icons.get(BadgeColor::for_state(&state))));
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_autostart::init(
            tauri_plugin_autostart::MacosLauncher::LaunchAgent,
            None,
        ))
        // Native file picker for Calendar Source's "Import sharing file"
        // (commands.rs's import_calendar_xml) — pc-app.md.
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_notification::init())
        .manage(ble::BleState::default())
        .manage(weather::WeatherState::default())
        .manage(holiday::HolidayState::default())
        .manage(reminders::RemindersState::default())
        .manage(notif_filter::NotifFilterState::default())
        .manage(store::StoreLock::default())
        .setup(|app| {
            let base_icon = app.default_window_icon().cloned();
            let mut tray_builder = TrayIconBuilder::with_id(TRAY_ID).tooltip("Orion");
            if let Some(icon) = &base_icon {
                let icons = TrayBadgeIcons::build(icon);
                // Matches `get_initial_state()`'s default ("off") so there's
                // no flash of an unbadged icon before the frontend's first
                // `setConn()` call lands.
                tray_builder = tray_builder.icon(icons.get(BadgeColor::Grey));
                app.manage(icons);
            }
            tray_builder
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        if let Some(window) = tray.app_handle().get_webview_window("main") {
                            toggle_panel(&window);
                        }
                    }
                })
                .build(app)?;

            // Calendar Source's background ICS poll (pc-app.md) — spawned
            // once here regardless of whether a source is configured yet;
            // see calendar_import::spawn_poll_task's own doc comment for why
            // this is the only spawn site.
            calendar_import::spawn_poll_task(app.handle().clone());

            // Weather badge's own background poll (pc-app.md/weather.rs) —
            // spawned once here regardless of whether a location has been
            // resolved yet; `weather::refresh()`'s own no-op-until-resolved
            // check handles that, same reasoning as the calendar poll above.
            weather::spawn_poll_task(app.handle().clone());

            // Local holiday support (pc-app.md, holiday.rs) — eagerly computes
            // the Tet lunar-date table so it's ready before the first BLE
            // (re)connect. holiday_country itself resolves later, from
            // weather::set_location()'s tail, once geolocation completes.
            holiday::init(app.handle());

            // Merged 60s tick for reminders.rs's end-of-day rain/snow
            // reminder and notif_filter.rs's Auto Do-Not-Disturb schedule —
            // spawned once here regardless of whether either has been
            // configured yet; each tick fn's own guards handle "nothing
            // configured" as a cheap no-op, same reasoning as the poll tasks
            // above. Previously two independent spawned loops, each doing
            // its own `store::load` every minute (a `spawn_blocking` file
            // read that can carry up to ~950 KB of profile/Time Off photo
            // data); loading once here and sharing it halves that redundant
            // disk I/O. notif_filter's boundary-crossing catch-up is the
            // only thing this loop is for on that side — every (re)connect
            // is covered separately by `start_post_sync_tasks`'s own
            // push_last_known call.
            {
                let app_handle = app.handle().clone();
                tauri::async_runtime::spawn(async move {
                    loop {
                        if let Some(saved) = store::load(&app_handle).await {
                            reminders::tick(&app_handle, &saved).await;
                            notif_filter::tick(&app_handle, &saved).await;
                        }
                        tokio::time::sleep(std::time::Duration::from_secs(60)).await;
                    }
                });
            }

            // Bluetooth guard (pc-app.md, ble/bt_radio.rs) — real "is the
            // radio on" monitoring, independent of pairing state (it must
            // catch Bluetooth going off even before any device is paired).
            ble::bt_radio::spawn_monitor(app.handle().clone());

            #[cfg(target_os = "windows")]
            if let Some(window) = app.get_webview_window("main") {
                apply_rounded_corners(&window);
                // The window itself already resizes correctly when dragged to a
                // different-DPI monitor — Tauri/WebView2 handle Per-Monitor-V2
                // scaling automatically, keeping the same logical (348x810)
                // footprint. What does NOT auto-adjust is the GDI clip region
                // above: `SetWindowRgn` is a fixed rectangle in physical pixels
                // at the DPI it was computed for, so after a monitor move
                // resizes the HWND to match the new physical scale, the old
                // region either clips real content or leaves the corners
                // wrong-radius.
                //
                // Re-apply on `Resized`, NOT `ScaleFactorChanged` — tao fires
                // `ScaleFactorChanged` *before* it actually resizes the HWND
                // (the event's `new_inner_size` is technically overridable by
                // the app, so tao has to ask first). Reading `outer_size()`
                // from that handler returns the stale pre-resize dimensions,
                // which was the original bug reintroduced one step later:
                // WebView2 repaints its content at the new DPI immediately,
                // but the clip mask got pinned to the old, smaller physical
                // size, cropping the now-larger content. `Resized` fires once
                // the HWND has actually changed size (whether from a DPI
                // change or anything else), so `outer_size()` there reflects
                // the true current size.
                //
                // Also re-applied on `Focused(true)` — a second, GUARANTEED
                // corrective pass the `Resized` hook alone can't provide.
                // `Resized` only fires for the "moved to a different-DPI
                // monitor" case above; it has no reason to fire at all for a
                // `resizable:false` window that never leaves its starting
                // monitor, so the very first `.setup()`-time call had no
                // self-correction if `scale_factor()` was itself still
                // unsettled at that instant (see apply_rounded_corners' own
                // comment on the outer_size()-removal fix this pairs with).
                // `Focused(true)` fires on every normal launch with no user
                // action needed (tauri.conf.json sets focus:true) and, by
                // the time the OS actually activates the window, its
                // monitor/DPI association is fully resolved — so this is a
                // corrective pass that doesn't depend on an event with no
                // firing guarantee.
                let corner_window = window.clone();
                window.on_window_event(move |event| match event {
                    tauri::WindowEvent::Resized(_) => apply_rounded_corners(&corner_window),
                    tauri::WindowEvent::Focused(true) => apply_rounded_corners(&corner_window),
                    _ => {}
                });
            }

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            set_tray_status,
            commands::get_initial_state,
            commands::hide_panel,
            commands::show_and_focus_window,
            commands::is_panel_visible,
            commands::show_native_notification,
            commands::force_reconnect,
            commands::ble_scan,
            commands::ble_start_pairing,
            commands::ble_cancel_pairing,
            commands::ble_submit_passkey,
            commands::save_profile,
            commands::save_timeoff,
            commands::clear_timeoff,
            commands::save_device_settings,
            commands::read_device_settings,
            commands::save_work_hours,
            commands::save_weather_alert,
            commands::save_low_battery_alert,
            commands::save_notif_filter_schedule,
            commands::get_ori_info,
            commands::get_shortcut_combos,
            commands::save_shortcuts,
            commands::unpair_phone,
            commands::ancs_notification_action,
            commands::set_calendar_source,
            commands::import_calendar_xml,
            commands::set_weather_location,
            commands::clear_all,
            commands::firmware_install,
            commands::orion_update_install,
            commands::orion_restart,
            commands::get_autostart_enabled,
            commands::set_autostart_enabled,
            commands::set_language,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
