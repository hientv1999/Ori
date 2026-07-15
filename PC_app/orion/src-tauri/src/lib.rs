mod ble;
mod commands;
mod store;

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
    let Ok(size) = window.outer_size() else { return };
    let scale = window.scale_factor().unwrap_or(1.0);
    let css_radius = 19.5 * scale; // matches --r in styles.css
    let clip_radius = (css_radius + 12.0 * scale).round() as i32;
    let region = unsafe {
        CreateRoundRectRgn(0, 0, size.width as i32 + 1, size.height as i32 + 1, clip_radius, clip_radius)
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
fn badge_color(state: &str) -> (u8, u8, u8) {
    match state {
        "on" => (0x92, 0xC3, 0x53),
        // "connecting" (found Ori, establishing the link) shares the same
        // amber as "rec" (Syncing) — matching styles.css's
        // `.h-dot.rec,.h-dot.connecting`. Without this arm, "connecting" fell
        // into the `_` default below and looked identical to fully
        // disconnected on the tray icon, the primary UI while the panel is
        // closed.
        "rec" | "connecting" => (0xFF, 0xAA, 0x44),
        _ => (0x8A, 0x88, 0x84),
    }
}

// Draws a filled circle (status color) over a thin dark outline circle in
// the icon's bottom-right corner, onto a copy of the base icon's raw RGBA —
// the outline keeps the dot legible against light or dark taskbar themes.
fn badge_icon(base: &Image<'_>, state: &str) -> Image<'static> {
    let width = base.width();
    let height = base.height();
    let mut rgba = base.rgba().to_vec();
    let (r, g, b) = badge_color(state);

    let min_dim = width.min(height) as f32;
    let radius = min_dim * 0.20;
    let outline = radius + (min_dim * 0.05).max(1.0);
    let cx = width as f32 - radius - 1.0;
    let cy = height as f32 - radius - 1.0;

    for y in 0..height {
        for x in 0..width {
            let dx = x as f32 + 0.5 - cx;
            let dy = y as f32 + 0.5 - cy;
            let dist = (dx * dx + dy * dy).sqrt();
            if dist > outline {
                continue;
            }
            let idx = ((y * width + x) * 4) as usize;
            if dist <= radius {
                rgba[idx] = r;
                rgba[idx + 1] = g;
                rgba[idx + 2] = b;
                rgba[idx + 3] = 255;
            } else {
                rgba[idx] = 0x1a;
                rgba[idx + 1] = 0x1a;
                rgba[idx + 2] = 0x1a;
                rgba[idx + 3] = 255;
            }
        }
    }

    Image::new_owned(rgba, width, height)
}

// Mirrors the panel's own `setConn()` — called from `app.js` on every
// connection-state transition (on/rec/off) so the tray icon never drifts
// from what the panel already shows.
#[tauri::command]
fn set_tray_status(app: AppHandle, state: String) {
    let Some(tray) = app.tray_by_id(TRAY_ID) else { return };
    let Some(base) = app.default_window_icon().cloned() else { return };
    let _ = tray.set_icon(Some(badge_icon(&base, &state)));
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_autostart::init(
            tauri_plugin_autostart::MacosLauncher::LaunchAgent,
            None,
        ))
        .manage(ble::BleState::default())
        .setup(|app| {
            let base_icon = app.default_window_icon().cloned();
            let mut tray_builder = TrayIconBuilder::with_id(TRAY_ID).tooltip("Orion");
            if let Some(icon) = &base_icon {
                // Matches `get_initial_state()`'s default ("off") so there's
                // no flash of an unbadged icon before the frontend's first
                // `setConn()` call lands.
                tray_builder = tray_builder.icon(badge_icon(icon, "off"));
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
                let corner_window = window.clone();
                window.on_window_event(move |event| {
                    if let tauri::WindowEvent::Resized(_) = event {
                        apply_rounded_corners(&corner_window);
                    }
                });
            }

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            set_tray_status,
            commands::get_initial_state,
            commands::hide_panel,
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
            commands::get_ori_info,
            commands::get_shortcut_combos,
            commands::save_shortcuts,
            commands::unpair_phone,
            commands::ancs_notification_action,
            commands::set_calendar_source,
            commands::oauth_google,
            commands::oauth_microsoft,
            commands::oauth_signout,
            commands::factory_reset,
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
