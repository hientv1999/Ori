// Media-mode OS bridge (Windows) — ble-protocol.md §12. Bridges Ori's
// KeyboardCommand notifies (char 000A) to OS actions, and reads OS
// media/volume state to push back as MediaMetadata / HostVolumeState.
//
// Command dispatch (Ori -> OS) deliberately does NOT use GSMTC's
// TryTogglePlayPauseAsync/TrySkipNextAsync/TrySkipPreviousAsync — §12's
// command table calls for simulating the global VK_MEDIA_* keys via
// keybd_event instead, since that works with whatever app has focus /
// registered global media-key hooks, not just apps that implement the SMTC
// integration. GSMTC is used only for the READ side (querying now-playing
// metadata to push to Ori) and for `seek`, which has no global-key
// equivalent.
//
// Media Album Art: the now-playing thumbnail is read via
// `Storage::Streams::DataReader::ReadBytes(&mut [u8])`, which turns out to
// need no unsafe COM buffer access at all (no `IBufferByteAccess`) — it's a
// safe windows-rs binding that copies straight into a Rust slice. Whatever
// format the OS hands back (JPEG/PNG/BMP in practice) is decoded, cropped
// to 484:216, resized, and re-encoded as JPEG under Ori's 64 KB cap via the
// `image` crate (ble-protocol.md §10/§12) — see `central.rs`'s
// `build_album_art_jpeg`.
//
// macOS bridge (private `MediaRemote` framework) is out of scope — that
// build hasn't started (memory.md).

/// Which class of GSMTC change woke `media_bridge`'s now-playing branch —
/// lets the caller (`central.rs`'s `check_and_push_now_playing`) decide
/// whether it's worth re-checking album art at all. Only `Properties` can
/// ever carry a thumbnail change: `MediaPropertiesChanged` bundles title/
/// artist/album/thumbnail together with no way to tell which sub-field
/// actually moved (so a genuine art-only update — a thumbnail arriving in a
/// second wave after the initial title/artist, or a stream rotating art
/// while its title/artist stay static — still shows up here), whereas
/// `PlaybackInfoChanged`/`TimelinePropertiesChanged` are play-state/position
/// only and never include artwork per the GSMTC API's own scope. A new
/// current session (`CurrentSessionChanged`) is also tagged `Properties` —
/// a different app/track entirely must recheck everything, art included.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum NowPlayingChange {
    Properties,
    PlaybackOrTimeline,
}

#[cfg(target_os = "windows")]
mod imp {
    use windows::Foundation::TypedEventHandler;
    use windows::Media::Control::{
        CurrentSessionChangedEventArgs, GlobalSystemMediaTransportControlsSession,
        GlobalSystemMediaTransportControlsSessionManager, GlobalSystemMediaTransportControlsSessionPlaybackStatus,
        MediaPropertiesChangedEventArgs, PlaybackInfoChangedEventArgs, TimelinePropertiesChangedEventArgs,
    };
    use windows::core::implement;
    use windows::Storage::Streams::DataReader;
    use windows::Win32::Foundation::PROPERTYKEY;
    use windows::Win32::Media::Audio::Endpoints::{
        IAudioEndpointVolume, IAudioEndpointVolumeCallback, IAudioEndpointVolumeCallback_Impl,
    };
    use windows::Win32::Media::Audio::{
        eCapture, eConsole, eRender, EDataFlow, ERole, DEVICE_STATE, AUDIO_VOLUME_NOTIFICATION_DATA, IMMDeviceEnumerator,
        IMMNotificationClient, IMMNotificationClient_Impl, MMDeviceEnumerator,
    };
    use windows::core::PCWSTR;
    use windows::Win32::System::Com::{CoCreateInstance, CoInitializeEx, CLSCTX_ALL, COINIT_MULTITHREADED};
    use windows::Win32::System::Shutdown::LockWorkStation;
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        keybd_event, VkKeyScanW, KEYEVENTF_KEYUP, VIRTUAL_KEY, VK_BACK, VK_CONTROL, VK_DELETE, VK_DOWN, VK_END, VK_F1,
        VK_HOME, VK_INSERT, VK_LEFT, VK_LWIN, VK_MEDIA_NEXT_TRACK, VK_MEDIA_PLAY_PAUSE, VK_MEDIA_PREV_TRACK, VK_MENU,
        VK_NEXT, VK_PRIOR, VK_RETURN, VK_RIGHT, VK_S, VK_SHIFT, VK_SPACE, VK_TAB, VK_UP,
    };

    fn ensure_com() {
        // Safe to call repeatedly from any thread this bridge runs on —
        // S_FALSE (already initialized, same apartment) is expected and
        // fine; only a hard failure would be a problem, and we don't have
        // a good recovery for that anyway (see pairing.rs for the same
        // pattern).
        unsafe {
            let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        }
    }

    fn press_key(vk: VIRTUAL_KEY) {
        unsafe {
            keybd_event(vk.0 as u8, 0, Default::default(), 0);
            keybd_event(vk.0 as u8, 0, KEYEVENTF_KEYUP, 0);
        }
    }

    /// `token` like "F1".."F24" (the exact, unmodified strings a function-key
    /// `keydown` produces — app.js's `_onKbdKey` passes `e.key` through
    /// unchanged for anything that isn't one of its explicitly-handled
    /// specials) → the matching virtual-key constant. `VK_F1..VK_F24` are
    /// contiguous in Win32, so this is one offset rather than 24 match arms.
    fn function_key_to_vk(token: &str) -> Option<VIRTUAL_KEY> {
        let n: u16 = token.strip_prefix('F')?.parse().ok()?;
        if (1..=24).contains(&n) {
            Some(VIRTUAL_KEY(VK_F1.0 + (n - 1)))
        } else {
            None
        }
    }

    /// Maps a single combo-recorder token (app.js's `_onKbdKey`) to its
    /// Windows virtual-key code. Letters and digits map directly — Windows
    /// VK codes equal their uppercase ASCII values, layout-independent.
    /// Named specials (arrows, Home/End, etc.) and function keys are
    /// explicit constants. Anything else (punctuation/symbols) goes through
    /// `VkKeyScanW` for the key identity only — its shift-required bit is
    /// deliberately ignored, since the combo's own explicit "Shift" modifier
    /// (if present) already reflects what the user actually held down while
    /// recording.
    fn key_token_to_vk(token: &str) -> Option<VIRTUAL_KEY> {
        match token {
            "Space" => Some(VK_SPACE),
            "⌫" => Some(VK_BACK),
            "Del" => Some(VK_DELETE),
            "↵" => Some(VK_RETURN),
            "Tab" => Some(VK_TAB),
            "↑" => Some(VK_UP),
            "↓" => Some(VK_DOWN),
            "←" => Some(VK_LEFT),
            "→" => Some(VK_RIGHT),
            "Home" => Some(VK_HOME),
            "End" => Some(VK_END),
            "PgUp" => Some(VK_PRIOR),
            "PgDn" => Some(VK_NEXT),
            "Ins" => Some(VK_INSERT),
            _ => {
                if let Some(vk) = function_key_to_vk(token) {
                    return Some(vk);
                }
                let mut chars = token.chars();
                let ch = chars.next()?;
                if chars.next().is_some() {
                    return None; // not a single-character token — unrecognized
                }
                if ch.is_ascii_alphanumeric() {
                    Some(VIRTUAL_KEY(ch.to_ascii_uppercase() as u16))
                } else {
                    let scan = unsafe { VkKeyScanW(ch as u16) };
                    if scan == -1 {
                        None
                    } else {
                        Some(VIRTUAL_KEY((scan as u16) & 0xFF))
                    }
                }
            }
        }
    }

    /// Replays a recorded "Favorite" shortcut combo (media-mode.md/pc-app.md
    /// — the user-configured action for shortcut slots). `parts` is
    /// modifier names ("Ctrl"/"Alt"/"Shift"/"Win", any subset, in any order)
    /// followed by exactly one final key token, exactly as recorded by the
    /// Quick Actions settings subscreen. Holds every modifier down, taps the
    /// final key, then releases modifiers in reverse order.
    pub fn press_combo(parts: &[String]) -> Result<(), String> {
        let (key_token, modifiers) = parts.split_last().ok_or("empty combo")?;
        let key_vk = key_token_to_vk(key_token).ok_or_else(|| format!("unrecognized key: {key_token}"))?;
        let modifier_vks: Vec<VIRTUAL_KEY> = modifiers
            .iter()
            .filter_map(|m| match m.as_str() {
                "Ctrl" => Some(VK_CONTROL),
                "Alt" => Some(VK_MENU),
                "Shift" => Some(VK_SHIFT),
                "Win" => Some(VK_LWIN),
                _ => None,
            })
            .collect();

        unsafe {
            for &vk in &modifier_vks {
                keybd_event(vk.0 as u8, 0, Default::default(), 0);
            }
            keybd_event(key_vk.0 as u8, 0, Default::default(), 0);
            keybd_event(key_vk.0 as u8, 0, KEYEVENTF_KEYUP, 0);
            for &vk in modifier_vks.iter().rev() {
                keybd_event(vk.0 as u8, 0, KEYEVENTF_KEYUP, 0);
            }
        }
        Ok(())
    }

    pub fn play_pause() {
        press_key(VK_MEDIA_PLAY_PAUSE);
    }

    pub fn next() {
        press_key(VK_MEDIA_NEXT_TRACK);
    }

    pub fn prev() {
        press_key(VK_MEDIA_PREV_TRACK);
    }

    /// Win+Shift+S — opens the Snipping Tool's region-capture overlay.
    pub fn trigger_screenshot() {
        unsafe {
            keybd_event(VK_LWIN.0 as u8, 0, Default::default(), 0);
            keybd_event(VK_SHIFT.0 as u8, 0, Default::default(), 0);
            keybd_event(VK_S.0 as u8, 0, Default::default(), 0);
            keybd_event(VK_S.0 as u8, 0, KEYEVENTF_KEYUP, 0);
            keybd_event(VK_SHIFT.0 as u8, 0, KEYEVENTF_KEYUP, 0);
            keybd_event(VK_LWIN.0 as u8, 0, KEYEVENTF_KEYUP, 0);
        }
    }

    pub fn lock_screen() -> Result<(), String> {
        unsafe { LockWorkStation() }.map_err(|e| e.to_string())
    }

    pub fn open_calculator() -> Result<(), String> {
        std::process::Command::new("calc.exe")
            .spawn()
            .map(|_| ())
            .map_err(|e| e.to_string())
    }

    /// `IMMDeviceEnumerator` is a factory/registry object, not a handle to any
    /// particular device — asking it for the default endpoint always
    /// reflects whatever's current, so caching the enumerator itself (rather
    /// than the endpoint it returns) is safe: it doesn't go stale when the
    /// user changes their default output/input device, and it saves a
    /// `CoCreateInstance` (COM class-factory lookup + object creation) on
    /// every single volume read — which `media_bridge`'s fallback poll does
    /// every `VOLUME_POLL_INTERVAL` for the whole life of a connected session.
    ///
    /// Cached per-thread (not in a shared global): windows-rs deliberately
    /// doesn't implement `Send`/`Sync` for this Win32 COM wrapper (unlike
    /// the WinRT `GlobalSystemMediaTransportControlsSessionManager` above,
    /// which does) — it wraps a raw `IUnknown` pointer with no generic
    /// guarantee about the underlying object's threading model, so the crate
    /// doesn't assert one. A `thread_local` sidesteps that entirely (no
    /// cross-thread sharing, so no `Send`/`Sync` bound needed) while still
    /// turning "one `CoCreateInstance` per call" into "one per worker
    /// thread" for Tokio's small, long-lived thread pool.
    fn device_enumerator() -> Result<IMMDeviceEnumerator, String> {
        thread_local! {
            static ENUMERATOR: std::cell::RefCell<Option<IMMDeviceEnumerator>> = const { std::cell::RefCell::new(None) };
        }
        ENUMERATOR.with(|cell| {
            if let Some(existing) = cell.borrow().as_ref() {
                return Ok(existing.clone());
            }
            let created: IMMDeviceEnumerator =
                unsafe { CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL) }.map_err(|e| format!("MMDeviceEnumerator: {e}"))?;
            *cell.borrow_mut() = Some(created.clone());
            Ok(created)
        })
    }

    fn endpoint_volume(flow: EDataFlow) -> Result<IAudioEndpointVolume, String> {
        // Must still run on every call (cheap — a no-op after the first
        // call on a given thread): joins *this* thread to the MTA even
        // though the cached enumerator above may have been created on a
        // different one.
        ensure_com();
        let enumerator = device_enumerator()?;
        unsafe {
            let device = enumerator
                .GetDefaultAudioEndpoint(flow, eConsole)
                .map_err(|e| format!("GetDefaultAudioEndpoint: {e}"))?;
            device
                .Activate::<IAudioEndpointVolume>(CLSCTX_ALL, None)
                .map_err(|e| format!("Activate IAudioEndpointVolume: {e}"))
        }
    }

    /// 0..100, rounded. `None` on a device with no volume-capable render
    /// endpoint (rare, but not worth panicking over).
    pub fn get_master_volume_percent() -> Result<u8, String> {
        let vol = endpoint_volume(eRender)?;
        let scalar = unsafe { vol.GetMasterVolumeLevelScalar() }.map_err(|e| e.to_string())?;
        Ok((scalar * 100.0).round().clamp(0.0, 100.0) as u8)
    }

    pub fn set_master_volume_percent(percent: u8) -> Result<(), String> {
        let vol = endpoint_volume(eRender)?;
        unsafe { vol.SetMasterVolumeLevelScalar(percent.min(100) as f32 / 100.0, std::ptr::null()) }.map_err(|e| e.to_string())
    }

    pub fn is_master_muted() -> Result<bool, String> {
        let vol = endpoint_volume(eRender)?;
        unsafe { vol.GetMute() }.map(|b| b.as_bool()).map_err(|e| e.to_string())
    }

    /// Returns the new mute state.
    pub fn toggle_master_mute() -> Result<bool, String> {
        let vol = endpoint_volume(eRender)?;
        unsafe {
            let new_state = !vol.GetMute().map_err(|e| e.to_string())?.as_bool();
            vol.SetMute(new_state, std::ptr::null()).map_err(|e| e.to_string())?;
            Ok(new_state)
        }
    }

    /// Returns the new mute state.
    pub fn toggle_mic_mute() -> Result<bool, String> {
        let vol = endpoint_volume(eCapture)?;
        unsafe {
            let new_state = !vol.GetMute().map_err(|e| e.to_string())?.as_bool();
            vol.SetMute(new_state, std::ptr::null()).map_err(|e| e.to_string())?;
            Ok(new_state)
        }
    }

    /// Fires `OnNotify` on every master-volume-level or mute change on the
    /// render endpoint it's registered against — WASAPI's own push
    /// notification, replacing `media_bridge`'s old fixed-interval poll.
    /// Just pings `tx`; the receiver re-fetches the actual level/mute state
    /// via `get_master_volume_percent`/`is_master_muted`, same as every
    /// other "ping, then re-fetch full state" pattern in this file
    /// (`watch_now_playing`).
    #[implement(IAudioEndpointVolumeCallback)]
    struct VolumeCallback {
        tx: tokio::sync::mpsc::UnboundedSender<()>,
    }

    impl IAudioEndpointVolumeCallback_Impl for VolumeCallback_Impl {
        fn OnNotify(&self, _pnotify: *mut AUDIO_VOLUME_NOTIFICATION_DATA) -> windows_core::Result<()> {
            let _ = self.tx.send(());
            Ok(())
        }
    }

    /// Registers `VolumeCallback` on whichever render endpoint is currently
    /// default. `None` (not an error) if there's currently no volume-capable
    /// render endpoint — `run_volume_watcher_blocking`'s loop just keeps
    /// waiting for the next device-change ping rather than giving up
    /// entirely.
    fn register_volume_callback(tx: &tokio::sync::mpsc::UnboundedSender<()>) -> Option<(IAudioEndpointVolume, IAudioEndpointVolumeCallback)> {
        let vol = endpoint_volume(eRender).ok()?;
        let callback: IAudioEndpointVolumeCallback = VolumeCallback { tx: tx.clone() }.into();
        unsafe { vol.RegisterControlChangeNotify(&callback) }.ok()?;
        Some((vol, callback))
    }

    enum VolumeWatcherMsg {
        DeviceChanged,
        Stop,
    }

    /// `IAudioEndpointVolumeCallback` is registered against ONE specific
    /// render endpoint, not "whatever's default" — unlike the poll it
    /// replaces, which re-resolved the default endpoint fresh on every
    /// single tick via `endpoint_volume(eRender)`, a one-time registration
    /// would silently go stale the moment the user's default output device
    /// changes (e.g. plugging in headphones). This client re-registers
    /// `VolumeCallback` onto the new endpoint whenever
    /// `OnDefaultDeviceChanged` reports the render/console role changed,
    /// preserving that same "always tracks whatever's current" behavior.
    #[implement(IMMNotificationClient)]
    struct DefaultDeviceListener {
        tx: std::sync::mpsc::Sender<VolumeWatcherMsg>,
    }

    impl IMMNotificationClient_Impl for DefaultDeviceListener_Impl {
        fn OnDeviceStateChanged(&self, _id: &PCWSTR, _state: DEVICE_STATE) -> windows_core::Result<()> {
            Ok(())
        }
        fn OnDeviceAdded(&self, _id: &PCWSTR) -> windows_core::Result<()> {
            Ok(())
        }
        fn OnDeviceRemoved(&self, _id: &PCWSTR) -> windows_core::Result<()> {
            Ok(())
        }
        fn OnDefaultDeviceChanged(&self, flow: EDataFlow, role: ERole, _id: &PCWSTR) -> windows_core::Result<()> {
            if flow == eRender && role == eConsole {
                let _ = self.tx.send(VolumeWatcherMsg::DeviceChanged);
            }
            Ok(())
        }
        fn OnPropertyValueChanged(&self, _id: &PCWSTR, _key: &PROPERTYKEY) -> windows_core::Result<()> {
            Ok(())
        }
    }

    /// Runs entirely on one of tokio's blocking-pool threads, never as a
    /// normal async task — same reasoning as
    /// `pairing::pair_with_passkey_blocking`'s module-level comment: the
    /// WASAPI/MMDevice COM objects involved here (`IAudioEndpointVolume`,
    /// `IMMDeviceEnumerator`, the callback objects themselves) aren't
    /// `Send`, so they can't survive across a normal async task's `.await`
    /// suspension points. A `spawn_blocking` closure runs to completion on
    /// one fixed OS thread, so holding them as plain local variables across
    /// this function's own *synchronous* blocking `Receiver::recv()` wait is
    /// fine — no `Send` bound applies to anything that never crosses an
    /// actual `.await`.
    ///
    /// Runs until a `Stop` message arrives (sent by `VolumeWatcher::drop`),
    /// then unregisters both the volume callback and the device-change
    /// listener before returning.
    fn run_volume_watcher_blocking(
        push_tx: tokio::sync::mpsc::UnboundedSender<()>,
        msg_tx: std::sync::mpsc::Sender<VolumeWatcherMsg>,
        msg_rx: std::sync::mpsc::Receiver<VolumeWatcherMsg>,
    ) {
        ensure_com();
        let Ok(enumerator) = device_enumerator() else { return };

        let listener: IMMNotificationClient = DefaultDeviceListener { tx: msg_tx }.into();
        let _ = unsafe { enumerator.RegisterEndpointNotificationCallback(&listener) };

        let mut current = register_volume_callback(&push_tx);

        // Exits on `Ok(Stop)` or `Err` (sender dropped) alike — neither
        // matches `Ok(DeviceChanged)`, so both simply end the loop.
        while let Ok(VolumeWatcherMsg::DeviceChanged) = msg_rx.recv() {
            if let Some((vol, callback)) = current.take() {
                let _ = unsafe { vol.UnregisterControlChangeNotify(&callback) };
            }
            current = register_volume_callback(&push_tx);
            // The switch itself may be a real volume change (a different
            // device, likely a different level) — let media_bridge's own
            // diff decide whether it's worth pushing rather than assuming
            // here.
            let _ = push_tx.send(());
        }

        if let Some((vol, callback)) = current.take() {
            let _ = unsafe { vol.UnregisterControlChangeNotify(&callback) };
        }
        let _ = unsafe { enumerator.UnregisterEndpointNotificationCallback(&listener) };
    }

    /// Keeps the volume-change watcher alive for as long as `media_bridge`
    /// holds this — signals `run_volume_watcher_blocking`'s loop to
    /// unregister and exit when dropped (connection ended). `stop_tx` is the
    /// only thing this struct holds — a plain `std::sync::mpsc::Sender`,
    /// safe to hold across `.await` points unlike the COM objects it
    /// controls on the other end.
    pub struct VolumeWatcher {
        stop_tx: std::sync::mpsc::Sender<VolumeWatcherMsg>,
    }

    impl Drop for VolumeWatcher {
        fn drop(&mut self) {
            let _ = self.stop_tx.send(VolumeWatcherMsg::Stop);
        }
    }

    /// Bridges WASAPI's own volume/mute-change and default-render-device-
    /// change notifications into pings on `tx`, so `media_bridge`'s loop can
    /// react to a real OS-level volume change instead of polling
    /// `get_master_volume_percent()` on a fixed timer — same idea as
    /// `watch_now_playing` for GSMTC now-playing changes.
    pub async fn watch_volume(tx: tokio::sync::mpsc::UnboundedSender<()>) -> Result<VolumeWatcher, String> {
        let (msg_tx, msg_rx) = std::sync::mpsc::channel::<VolumeWatcherMsg>();
        let stop_tx = msg_tx.clone();
        tokio::task::spawn_blocking(move || run_volume_watcher_blocking(tx, msg_tx, msg_rx));
        Ok(VolumeWatcher { stop_tx })
    }

    pub struct NowPlaying {
        pub title: String,
        pub artist: String,
        pub can_seek: bool,
        pub playing: bool,
        pub position_s: Option<u32>,
        pub duration_s: Option<u32>,
    }

    /// `RequestAsync()` is a broker round-trip, not a cheap getter — reused
    /// across calls instead of being re-awaited from scratch by every one of
    /// `now_playing`/`seek`/`now_playing_thumbnail`, since `media_bridge` calls
    /// `now_playing()` on every GSMTC-pushed change and, as a fallback when
    /// that watcher isn't active, every `VOLUME_POLL_INTERVAL` too — for the
    /// whole life of a connected session. The manager is a registry object —
    /// `GetCurrentSession()` always reflects whichever app is current right
    /// now, so caching the manager itself doesn't risk serving a stale
    /// track/session. `now_playing()` already held one of these across an
    /// `.await` before this change (see below), which is existing proof this
    /// type is `Send`-compatible with this file's async tasks.
    async fn session_manager() -> Result<GlobalSystemMediaTransportControlsSessionManager, String> {
        static MGR: std::sync::OnceLock<GlobalSystemMediaTransportControlsSessionManager> = std::sync::OnceLock::new();
        if let Some(existing) = MGR.get() {
            return Ok(existing.clone());
        }
        let created = GlobalSystemMediaTransportControlsSessionManager::RequestAsync()
            .map_err(|e| e.to_string())?
            .await
            .map_err(|e| e.to_string())?;
        Ok(MGR.get_or_init(|| created).clone())
    }

    /// `Ok(None)` = no active session (nothing playing) — a real state, not
    /// an error; the caller pushes an empty MediaMetadata for it.
    pub async fn now_playing() -> Result<Option<NowPlaying>, String> {
        let mgr = session_manager().await?;
        let Ok(session) = mgr.GetCurrentSession() else { return Ok(None) };

        let props = session
            .TryGetMediaPropertiesAsync()
            .map_err(|e| e.to_string())?
            .await
            .map_err(|e| e.to_string())?;
        let playback = session.GetPlaybackInfo().map_err(|e| e.to_string())?;
        let playing = playback.PlaybackStatus().map_err(|e| e.to_string())?
            == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        let can_seek = playback
            .Controls()
            .and_then(|c| c.IsPlaybackPositionEnabled())
            .unwrap_or(false);

        let mut position_s = None;
        let mut duration_s = None;
        if let Ok(timeline) = session.GetTimelineProperties() {
            if let Ok(end) = timeline.EndTime() {
                let secs = (end.Duration / 10_000_000).max(0) as u32;
                if secs > 0 {
                    duration_s = Some(secs);
                    position_s = timeline.Position().ok().map(|p| (p.Duration / 10_000_000).max(0) as u32);
                }
            }
        }

        Ok(Some(NowPlaying {
            title: props.Title().map(|s| s.to_string()).unwrap_or_default(),
            artist: props.Artist().map(|s| s.to_string()).unwrap_or_default(),
            can_seek,
            playing,
            position_s,
            duration_s,
        }))
    }

    pub async fn seek(seconds: u32) -> Result<(), String> {
        let mgr = session_manager().await?;
        let session = mgr.GetCurrentSession().map_err(|e| e.to_string())?;
        session
            .TryChangePlaybackPositionAsync((seconds as i64) * 10_000_000)
            .map_err(|e| e.to_string())?
            .await
            .map_err(|e| e.to_string())?;
        Ok(())
    }

    /// Raw thumbnail bytes (whatever format the OS hands back — JPEG/PNG/
    /// BMP in practice) for the current now-playing session, or empty if
    /// there's no session or no thumbnail. `central.rs`'s
    /// `build_album_art_jpeg` handles decoding/resizing/re-encoding.
    pub async fn now_playing_thumbnail() -> Result<Vec<u8>, String> {
        let mgr = session_manager().await?;
        let Ok(session) = mgr.GetCurrentSession() else { return Ok(Vec::new()) };
        let props = session
            .TryGetMediaPropertiesAsync()
            .map_err(|e| e.to_string())?
            .await
            .map_err(|e| e.to_string())?;
        // Scoped so the non-Send WinRT handles (`props`, `thumb_ref`) are
        // dropped before the `.await` below — otherwise the compiler holds
        // them live across it and the whole future stops being Send, which
        // `tokio::spawn` (in `media_bridge`) requires.
        let open_op = {
            let Ok(thumb_ref) = props.Thumbnail() else { return Ok(Vec::new()) };
            match thumb_ref.OpenReadAsync() {
                Ok(op) => op,
                Err(_) => return Ok(Vec::new()),
            }
        };
        // Same non-Send-across-await issue as `props`/`thumb_ref` above —
        // `stream` must be *declared* inside this block (not just used
        // here after being declared outside it) for its scope, and thus
        // its drop, to end before the next `.await`. A bare `drop(stream)`
        // placed after an outer declaration does not achieve the same
        // thing for the generator's Send computation.
        let (size, reader) = {
            let Ok(stream) = open_op.await else { return Ok(Vec::new()) };
            let size = stream.Size().map_err(|e| e.to_string())?;
            let reader = DataReader::CreateDataReader(&stream).map_err(|e| e.to_string())?;
            (size, reader)
        };
        // 0 = no thumbnail; sanity-cap against a pathological stream size
        // (real album art thumbnails are at most a few MB).
        if size == 0 || size > 20_000_000 {
            return Ok(Vec::new());
        }
        reader.LoadAsync(size as u32).map_err(|e| e.to_string())?.await.map_err(|e| e.to_string())?;
        let mut buf = vec![0u8; size as usize];
        reader.ReadBytes(&mut buf).map_err(|e| e.to_string())?;
        Ok(buf)
    }

    /// Registers `session.MediaPropertiesChanged`/`PlaybackInfoChanged`/
    /// `TimelinePropertiesChanged` on `session`, each pinging `tx` tagged
    /// with the matching `NowPlayingChange` — the receiver re-fetches full
    /// state via `now_playing()` either way, but only re-checks album art on
    /// a `Properties` ping (see that enum's doc comment for why). Called
    /// both for the session that's current when watching starts and, from
    /// `watch_now_playing`'s own `CurrentSessionChanged` handler, for
    /// whichever session becomes current later — each session needs its own
    /// subscriptions, they aren't inherited from the manager.
    fn subscribe_session_events(
        session: &GlobalSystemMediaTransportControlsSession,
        tx: &tokio::sync::mpsc::UnboundedSender<super::NowPlayingChange>,
    ) {
        let tx1 = tx.clone();
        let media_props_handler = TypedEventHandler::<GlobalSystemMediaTransportControlsSession, MediaPropertiesChangedEventArgs>::new(
            move |_, _| {
                let _ = tx1.send(super::NowPlayingChange::Properties);
                Ok(())
            },
        );
        let _ = session.MediaPropertiesChanged(&media_props_handler);

        let tx2 = tx.clone();
        let playback_info_handler = TypedEventHandler::<GlobalSystemMediaTransportControlsSession, PlaybackInfoChangedEventArgs>::new(
            move |_, _| {
                let _ = tx2.send(super::NowPlayingChange::PlaybackOrTimeline);
                Ok(())
            },
        );
        let _ = session.PlaybackInfoChanged(&playback_info_handler);

        let tx3 = tx.clone();
        let timeline_handler = TypedEventHandler::<GlobalSystemMediaTransportControlsSession, TimelinePropertiesChangedEventArgs>::new(
            move |_, _| {
                let _ = tx3.send(super::NowPlayingChange::PlaybackOrTimeline);
                Ok(())
            },
        );
        let _ = session.TimelinePropertiesChanged(&timeline_handler);
    }

    /// Best-effort session identity, used only to detect "this is genuinely
    /// the same session we already subscribed to" (see `watch_now_playing`).
    /// `SourceAppUserModelId` is stable for the lifetime of a given app's
    /// SMTC session, unlike the session object itself, which GSMTC may hand
    /// back as a fresh COM wrapper on `GetCurrentSession()` even when
    /// nothing has actually changed. `None` if the property can't be read —
    /// callers fall back to the safe (always re-subscribe) behavior in that
    /// case rather than risk silently never subscribing to a later session.
    fn session_identity(session: &GlobalSystemMediaTransportControlsSession) -> Option<String> {
        session.SourceAppUserModelId().ok().map(|id| id.to_string())
    }

    /// Keeps `watch_now_playing`'s manager-level subscription alive for as
    /// long as `media_bridge` holds this — dropped (and the subscription
    /// removed) when the bridge task ends on disconnect.
    pub struct NowPlayingWatcher {
        mgr: GlobalSystemMediaTransportControlsSessionManager,
        mgr_token: i64,
    }

    impl Drop for NowPlayingWatcher {
        fn drop(&mut self) {
            let _ = self.mgr.RemoveCurrentSessionChanged(self.mgr_token);
        }
    }

    /// Bridges GSMTC's native change notifications into pings on `tx`, so
    /// `media_bridge`'s loop can react to a real change instead of polling
    /// `now_playing()` on a fixed timer. This matters because the dominant
    /// per-poll cost isn't the polling mechanism itself — it's
    /// `TryGetMediaPropertiesAsync()` inside `now_playing()`, which round-
    /// trips to the *source app's own process* for title/artist/thumbnail
    /// metadata, repeated on every tick whether or not anything changed.
    /// Firing only on genuine GSMTC-reported changes turns "dozens of
    /// cross-process calls per minute, forever" into "a handful per actual
    /// track/play-state change."
    ///
    /// `CurrentSessionChanged` is registered once, for the manager's
    /// lifetime (its token is held in the returned `NowPlayingWatcher` and
    /// removed on drop). Every time it fires — including once up front here,
    /// for whatever session is already current — the *session's own* three
    /// events are (re-)registered via `subscribe_session_events`, since
    /// those are per-session, not per-manager. Skipped when
    /// `session_identity` confirms the newly-current session is the same one
    /// already subscribed to (see below).
    ///
    /// Past sessions' three subscriptions are not explicitly removed: doing
    /// so needs the old session object to call `RemoveXChanged` on, and by
    /// the time a new one replaces it, we've already let go of it — the OS
    /// releases a session (and everything registered on it) once the app
    /// that owns it ends its own SMTC session. That's a genuinely bounded,
    /// one-time cost per past app/track *source* the user switches away from
    /// for good. What it doesn't cover on its own is a user repeatedly
    /// switching *back and forth* between two still-alive sessions (e.g.
    /// Spotify <-> a browser tab) without either ending — GSMTC hands back
    /// the *same* underlying session each time it becomes current again, so
    /// re-subscribing unconditionally on every `CurrentSessionChanged` would
    /// pile up a fresh set of duplicate registrations on that same session
    /// per switch, unbounded by anything except how many times the user
    /// alternates. `last_session_id` below closes that gap: a
    /// `CurrentSessionChanged` firing for a session we already have live
    /// subscriptions on is a no-op rather than another round of registrations.
    pub async fn watch_now_playing(tx: tokio::sync::mpsc::UnboundedSender<super::NowPlayingChange>) -> Result<NowPlayingWatcher, String> {
        let mgr = session_manager().await?;

        let last_session_id = std::sync::Arc::new(std::sync::Mutex::new(None::<String>));

        if let Ok(session) = mgr.GetCurrentSession() {
            // `unwrap_or_else(PoisonError::into_inner)` recovers the guard
            // even if some other holder of this lock ever panicked — this
            // Mutex only ever wraps a plain `Option<String>` behind a
            // trivial assignment/comparison (no I/O, nothing expected to
            // panic), so poisoning would be surprising, but there's no
            // reason to propagate an unrelated panic into this WinRT event
            // callback when recovering the last-known value is just as valid.
            *last_session_id.lock().unwrap_or_else(|e| e.into_inner()) = session_identity(&session);
            subscribe_session_events(&session, &tx);
        }

        let mgr_for_handler = mgr.clone();
        let tx_for_handler = tx.clone();
        let last_session_id_for_handler = last_session_id.clone();
        let current_session_handler = TypedEventHandler::<GlobalSystemMediaTransportControlsSessionManager, CurrentSessionChangedEventArgs>::new(
            move |_, _| {
                if let Ok(session) = mgr_for_handler.GetCurrentSession() {
                    let id = session_identity(&session);
                    // See the identical PoisonError recovery above.
                    let mut last = last_session_id_for_handler.lock().unwrap_or_else(|e| e.into_inner());
                    // `id.is_none()` falls back to always re-subscribing —
                    // if the identity can't be read this time, we can't
                    // safely claim "same session, skip," and silently never
                    // subscribing to a later genuinely-new session would be
                    // worse than the duplicate-registration cost this is
                    // meant to avoid.
                    if id.is_none() || id != *last {
                        subscribe_session_events(&session, &tx_for_handler);
                        *last = id;
                    }
                }
                // A new current session is a different app/track entirely —
                // tagged Properties so the receiver rechecks art too, not
                // just metadata.
                let _ = tx_for_handler.send(super::NowPlayingChange::Properties);
                Ok(())
            },
        );
        let mgr_token = mgr.CurrentSessionChanged(&current_session_handler).map_err(|e| e.to_string())?;

        Ok(NowPlayingWatcher { mgr, mgr_token })
    }
}

#[cfg(not(target_os = "windows"))]
mod imp {
    // macOS bridge is out of scope — that build hasn't started (memory.md).
    pub fn play_pause() {}
    pub fn next() {}
    pub fn prev() {}
    pub fn trigger_screenshot() {}
    pub fn press_combo(_parts: &[String]) -> Result<(), String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub fn lock_screen() -> Result<(), String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub fn open_calculator() -> Result<(), String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub fn get_master_volume_percent() -> Result<u8, String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub fn set_master_volume_percent(_percent: u8) -> Result<(), String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub fn is_master_muted() -> Result<bool, String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub fn toggle_master_mute() -> Result<bool, String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub fn toggle_mic_mute() -> Result<bool, String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub struct VolumeWatcher;
    pub async fn watch_volume(_tx: tokio::sync::mpsc::UnboundedSender<()>) -> Result<VolumeWatcher, String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub struct NowPlaying {
        pub title: String,
        pub artist: String,
        pub can_seek: bool,
        pub playing: bool,
        pub position_s: Option<u32>,
        pub duration_s: Option<u32>,
    }
    pub async fn now_playing() -> Result<Option<NowPlaying>, String> {
        Ok(None)
    }
    pub async fn seek(_seconds: u32) -> Result<(), String> {
        Err("media bridge not implemented on this platform yet".into())
    }
    pub async fn now_playing_thumbnail() -> Result<Vec<u8>, String> {
        Ok(Vec::new())
    }
    pub struct NowPlayingWatcher;
    pub async fn watch_now_playing(_tx: tokio::sync::mpsc::UnboundedSender<super::NowPlayingChange>) -> Result<NowPlayingWatcher, String> {
        Err("media bridge not implemented on this platform yet".into())
    }
}

pub use imp::*;
