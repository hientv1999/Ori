mod commands;

use tauri::{
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    Manager, WebviewWindow,
};

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

// The panel stays visible until the user explicitly minimizes it (the
// in-app minimize button, or clicking the tray icon again) — it does NOT
// auto-hide on focus loss. Toggling preserves wherever the user last moved
// the window (no re-anchoring to the tray icon's click point).
fn toggle_panel(window: &WebviewWindow) {
    if window.is_visible().unwrap_or(false) {
        let _ = window.hide();
    } else {
        let _ = window.show();
        let _ = window.set_focus();
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_autostart::init(
            tauri_plugin_autostart::MacosLauncher::LaunchAgent,
            None,
        ))
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            let tray_icon = app.default_window_icon().cloned();
            let mut tray_builder = TrayIconBuilder::new().tooltip("Orion");
            if let Some(icon) = tray_icon {
                tray_builder = tray_builder.icon(icon);
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
            }

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::get_initial_state,
            commands::hide_panel,
            commands::ble_scan,
            commands::ble_pair,
            commands::save_profile,
            commands::save_timeoff,
            commands::clear_timeoff,
            commands::save_device_settings,
            commands::read_device_settings,
            commands::save_shortcuts,
            commands::set_calendar_source,
            commands::oauth_google,
            commands::oauth_microsoft,
            commands::oauth_signout,
            commands::factory_reset,
            commands::clear_all,
            commands::firmware_install,
            commands::orion_update_install,
            commands::orion_restart,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
