// Weather badge + temperature bubble (screen-layout.md's profile card) —
// Device Settings "w"/"d"/"u" fields (ble-protocol.md §4/§6.4). Fully
// automatic, no Settings UI and no user-entered location or unit —
// pc-app.md's periodic-refresh table lists this row's only trigger as
// "Weather-API poll," never "User changes setting," unlike Clock Face/Time
// Format/ANCS Filter, which all have a real Settings subscreen.
//
// Three network calls, all keyless (no signup, no API key, no OAuth — same
// "avoid registration friction" reasoning that ruled out Microsoft Graph
// for calendar, `calendar_import.rs`):
//   - Precise location: the frontend's own `navigator.geolocation` first
//     (real GPS/Wi-Fi positioning via the OS, through WebView2/WKWebView),
//     reported once via `set_weather_location` (commands.rs). Reverse-
//     geocoded via BigDataCloud (`api.bigdatacloud.net`, built specifically
//     for this "browser geolocation → country" pairing) to learn the
//     country for the temperature-unit heuristic below.
//   - IP-based fallback: ipapi.co, when the frontend reports geolocation
//     failed/was denied. Returns lat/lon AND country in one call, so no
//     separate reverse-geocode step is needed on this path.
//   - Weather itself: Open-Meteo (open-meteo.com) — free, keyless, current
//     temperature + a WMO weather code for a lat/lon.
//
// Resolved ONCE per app launch, not re-resolved periodically — Ori is a
// desk-based display and Orion its PC companion (product-intent.md); the
// desk doesn't move mid-session. Only the weather reading itself is
// re-polled, on the same cadence as `calendar_import.rs`'s ICS poll.
//
// Temperature unit is inferred once from the resolved location's country —
// the same short "uses Fahrenheit by default" country list most weather
// apps use — not user-configurable, matching the "fully automatic" design
// above. If this guess is ever wrong for someone, the fix is a real
// Settings toggle, not a workaround here.

use serde::Deserialize;
use tauri::Manager;
use tokio::sync::Mutex;

/// Countries that default to Fahrenheit — everyone else defaults to
/// Celsius. Standard short list (matches e.g. most weather widgets' own
/// default-unit heuristic): US, Bahamas, Belize, Cayman Islands, Liberia,
/// Palau, Federated States of Micronesia, Marshall Islands.
const FAHRENHEIT_COUNTRIES: &[&str] = &["US", "BS", "BZ", "KY", "LR", "PW", "FM", "MH"];

/// Ori's `DeviceSettings.weather_condition` enum (ble-protocol.md §4) — kept
/// as a real enum here (rather than a bare `u8`) so `map_wmo_code`'s match
/// arms stay self-documenting; converted to the wire `u8` only at the
/// `set_device_settings` call site.
#[derive(Clone, Copy, PartialEq, Debug)]
enum Condition {
    Clear = 0,
    PartlyCloudy = 1,
    Cloudy = 2,
    Rain = 3,
    Thunderstorm = 4,
    Snow = 5,
    Fog = 6,
}

/// Maps Open-Meteo's WMO (World Meteorological Organization) weather-
/// interpretation code to Ori's own 7-value enum — Ori has no "drizzle" or
/// "rain showers" bucket, so several WMO codes collapse onto the same Ori
/// condition. Reference: https://open-meteo.com/en/docs (WMO code table).
fn map_wmo_code(code: u32) -> Condition {
    match code {
        0 => Condition::Clear,
        1 => Condition::Clear,       // "mainly clear"
        2 => Condition::PartlyCloudy,
        3 => Condition::Cloudy,      // "overcast"
        45 | 48 => Condition::Fog,
        51 | 53 | 55 | 56 | 57 => Condition::Rain, // drizzle, incl. freezing
        61 | 63 | 65 | 66 | 67 => Condition::Rain, // rain, incl. freezing
        80..=82 => Condition::Rain, // rain showers
        71 | 73 | 75 | 77 => Condition::Snow,
        85 | 86 => Condition::Snow, // snow showers
        95 | 96 | 99 => Condition::Thunderstorm,
        _ => Condition::Cloudy, // unrecognized code — safest visual default
    }
}

/// Ori's `DeviceSettings.intensity` enum (ble-protocol.md §4) — how hard
/// precipitation/fog/storms are currently coming down, 0-3. Derived from the
/// SAME Open-Meteo WMO code as `map_wmo_code`, just bucketed into severity
/// instead of (or alongside) condition — no separate API field needed.
#[derive(Clone, Copy, PartialEq, Debug)]
enum Intensity {
    None = 0,
    Light = 1,
    Moderate = 2,
    Heavy = 3,
}

/// Maps Open-Meteo's WMO weather-interpretation code to Ori's own 4-value
/// intensity enum. Mirrors the same WMO table `map_wmo_code` switches on —
/// see https://open-meteo.com/en/docs — just producing a severity bucket
/// instead of a condition bucket. Anything not explicitly a rain/snow/
/// thunderstorm/fog code (clear, partly cloudy, overcast, or unrecognized)
/// has no meaningful "intensity" and maps to `None`.
fn map_wmo_intensity(code: u32) -> Intensity {
    match code {
        // Rain family — drizzle, rain (incl. freezing), rain showers
        51 | 56 | 61 | 66 | 80 => Intensity::Light,
        53 | 63 | 81 => Intensity::Moderate,
        55 | 57 | 65 | 67 | 82 => Intensity::Heavy,
        // Snow family
        71 | 77 | 85 => Intensity::Light,
        73 => Intensity::Moderate,
        75 | 86 => Intensity::Heavy,
        // Thunderstorm
        95 => Intensity::Light,
        96 => Intensity::Moderate,
        99 => Intensity::Heavy,
        // Fog
        45 => Intensity::Light,
        48 => Intensity::Heavy,
        _ => Intensity::None,
    }
}

/// A location resolved once per app session — see this module's own doc
/// comment for why it's resolved once rather than re-geolocated per poll.
/// `country_code` (raw ISO 3166-1 alpha-2, e.g. "US"/"VN") and `region_code`
/// (raw ISO 3166-2 subdivision code with any country prefix already
/// stripped, e.g. "BC"/"SCT" — see `strip_country_prefix()`) are kept around
/// (not just the derived `fahrenheit` bool) so holiday.rs can reuse the same
/// resolution instead of re-deriving a country/region of its own — see
/// `country_code()`/`region_code()` below.
#[derive(Clone, PartialEq)]
struct Location {
    lat: f64,
    lon: f64,
    fahrenheit: bool,
    country_code: String,
    region_code: String,
}

/// Exactly as last successfully written to Ori — `push_last_known` (called
/// on every reconnect) resends this as-is, with no fresh fetch, and the poll
/// loop only writes through `set_device_settings` when a fresh fetch
/// actually differs from this.
#[derive(Clone, Copy, PartialEq)]
struct WeatherReading {
    condition: u8,
    temperature: i32,
    unit: u8,
    is_night: bool,
    intensity: u8,
}

#[derive(Default)]
pub struct WeatherState {
    location: Mutex<Option<Location>>,
    last_pushed: Mutex<Option<WeatherReading>>,
}

#[derive(Deserialize)]
struct IpApiCoResponse {
    latitude: f64,
    longitude: f64,
    country_code: String,
    /// Bare subdivision code, no country prefix (e.g. "BC" for Canada,
    /// "CA" for California) — confirmed via a live call, not guessed.
    #[serde(default)]
    region_code: String,
}

#[derive(Deserialize)]
struct BigDataCloudResponse {
    #[serde(rename = "countryCode")]
    country_code: String,
    /// ISO 3166-2 code WITH the country prefix (e.g. "CA-BC") — confirmed
    /// via a live call. Stripped down to the bare code by
    /// `strip_country_prefix()` before being cached, so both providers end
    /// up in the same bare format.
    #[serde(rename = "principalSubdivisionCode", default)]
    principal_subdivision_code: String,
}

/// Normalizes a subdivision code to its bare form (no country prefix) —
/// BigDataCloud returns "CA-BC", ipapi.co returns "BC" directly. Splitting
/// on the last '-' handles both uniformly without needing to know the
/// country code separately.
fn strip_country_prefix(code: &str) -> &str {
    code.rsplit('-').next().unwrap_or(code)
}

#[derive(Deserialize)]
struct OpenMeteoResponse {
    current: OpenMeteoCurrent,
}

#[derive(Deserialize)]
struct OpenMeteoCurrent {
    temperature_2m: f64,
    weather_code: u32,
    /// 1 = daytime, 0 = nighttime — Open-Meteo's own sun-position flag,
    /// requested alongside temperature/weather_code so no second API call is
    /// needed for Ori's `DeviceSettings.is_night` field.
    is_day: u8,
}

/// Returns (country_code, region_code) — region_code is "" when the
/// provider has none for this location (e.g. a country with no subdivision
/// data, or a coastal/international point).
async fn reverse_geocode(lat: f64, lon: f64) -> Result<(String, String), String> {
    let url = format!(
        "https://api.bigdatacloud.net/data/reverse-geocode-client?latitude={lat}&longitude={lon}&localityLanguage=en"
    );
    let text = reqwest::get(&url).await.map_err(|e| format!("reverse-geocoding location: {e}"))?
        .text().await.map_err(|e| format!("reading reverse-geocode response: {e}"))?;
    let parsed: BigDataCloudResponse =
        serde_json::from_str(&text).map_err(|e| format!("parsing reverse-geocode response: {e}"))?;
    let region = strip_country_prefix(&parsed.principal_subdivision_code).to_string();
    Ok((parsed.country_code, region))
}

async fn geolocate_by_ip() -> Result<(f64, f64, String, String), String> {
    let text = reqwest::get("https://ipapi.co/json/").await.map_err(|e| format!("IP geolocation: {e}"))?
        .text().await.map_err(|e| format!("reading IP geolocation response: {e}"))?;
    let parsed: IpApiCoResponse =
        serde_json::from_str(&text).map_err(|e| format!("parsing IP geolocation response: {e}"))?;
    let region = strip_country_prefix(&parsed.region_code).to_string();
    Ok((parsed.latitude, parsed.longitude, parsed.country_code, region))
}

/// Resolves (and caches for the rest of this app session) the location used
/// for every subsequent weather poll. `lat`/`lon` come from the frontend's
/// `navigator.geolocation` call (commands.rs's `set_weather_location`) —
/// `Some` on success, `None` on denial/timeout/unsupported, in which case
/// this falls back to IP-based geolocation (which conveniently returns a
/// country directly, needing no separate reverse-geocode call).
///
/// Does an immediate weather fetch+push right after resolving, same as
/// `calendar_import::refresh()` does after a fresh XML import — so the
/// profile card gets its weather badge as soon as possible rather than
/// waiting for the first poll tick.
pub async fn set_location(app: &tauri::AppHandle, lat: Option<f64>, lon: Option<f64>) -> Result<(), String> {
    use tauri::Emitter;

    let (lat, lon, country, region) = match (lat, lon) {
        (Some(lat), Some(lon)) => {
            let (country, region) = reverse_geocode(lat, lon).await.unwrap_or_default();
            (lat, lon, country, region)
        }
        _ => match geolocate_by_ip().await {
            Ok(resolved) => resolved,
            Err(e) => {
                let _ = app.emit("network-health", serde_json::json!({"source": "weather", "ok": false}));
                return Err(e);
            }
        },
    };

    let fahrenheit = FAHRENHEIT_COUNTRIES.contains(&country.as_str());
    let location = Location { lat, lon, fahrenheit, country_code: country, region_code: region };
    let state = app.state::<WeatherState>();
    *state.location.lock().await = Some(location);

    // holiday.rs's Device Settings "g" field depends on this same country
    // resolution — trigger it the instant location resolves rather than
    // waiting on a separate geolocation call of its own (`country_code()`'s
    // own doc comment). Runs regardless of whether the weather fetch below
    // succeeds.
    crate::holiday::resolve_and_push(app).await;

    refresh(app).await
}

/// The raw ISO 3166-1 alpha-2 country code resolved by `set_location()` (e.g.
/// "US"/"VN"), or `None` if location hasn't resolved yet this session.
/// Reused by holiday.rs to pick a holiday_data::Country without re-deriving
/// its own location — same country, same resolution, no duplicate network
/// calls for two features that both just need "which country am I in."
pub async fn country_code(app: &tauri::AppHandle) -> Option<String> {
    let state = app.state::<WeatherState>();
    let country = state.location.lock().await.as_ref().map(|loc| loc.country_code.clone());
    country
}

/// The raw, bare (no country prefix) ISO 3166-2 subdivision code resolved by
/// `set_location()` (e.g. "BC", "SCT"), or `None` if location hasn't
/// resolved yet, or `Some("")` if the provider had no subdivision for this
/// location. Reused by holiday.rs the same way `country_code()` is.
pub async fn region_code(app: &tauri::AppHandle) -> Option<String> {
    let state = app.state::<WeatherState>();
    let region = state.location.lock().await.as_ref().map(|loc| loc.region_code.clone());
    region
}

/// One fetch-and-maybe-push cycle: re-fetches current weather for whatever
/// location `set_location` last resolved, and — only if the reading
/// actually differs from what was last successfully pushed — writes it to
/// Ori via `ble::set_device_settings` (same "already know it's needed, no
/// manifest round-trip" reasoning as `calendar_import.rs`'s `push_meetings`;
/// Device Settings needs no manifest at all, §6.4). A no-op, not an error,
/// when no location has been resolved yet (`set_location` hasn't completed,
/// or hasn't been called at all).
async fn refresh(app: &tauri::AppHandle) -> Result<(), String> {
    use tauri::Emitter;

    let state = app.state::<WeatherState>();
    let Some(location) = state.location.lock().await.clone() else {
        return Ok(()); // nothing resolved yet — no network attempt, nothing to report
    };

    let unit_param = if location.fahrenheit { "fahrenheit" } else { "celsius" };
    let url = format!(
        "https://api.open-meteo.com/v1/forecast?latitude={}&longitude={}&current=temperature_2m,weather_code,is_day&temperature_unit={unit_param}",
        location.lat, location.lon
    );
    let fetch_result = async {
        let text = reqwest::get(&url).await.map_err(|e| format!("fetching weather: {e}"))?
            .text().await.map_err(|e| format!("reading weather response: {e}"))?;
        serde_json::from_str::<OpenMeteoResponse>(&text).map_err(|e| format!("parsing weather response: {e}"))
    }
    .await;
    let _ = app.emit("network-health", serde_json::json!({"source": "weather", "ok": fetch_result.is_ok()}));
    let parsed = fetch_result?;

    let condition = map_wmo_code(parsed.current.weather_code) as u8;
    let temperature = parsed.current.temperature_2m.round() as i32;
    let unit: u8 = if location.fahrenheit { 0 } else { 1 };
    let intensity = map_wmo_intensity(parsed.current.weather_code) as u8;
    let is_night = parsed.current.is_day == 0;
    let reading = WeatherReading { condition, temperature, unit, is_night, intensity };

    let mut last_pushed = state.last_pushed.lock().await;
    if *last_pushed == Some(reading) {
        return Ok(());
    }
    *last_pushed = Some(reading);
    drop(last_pushed);

    let ble_state = app.state::<crate::ble::BleState>();
    // Best-effort — a failure here (most commonly "not connected right
    // now") just means the next reconnect's `push_last_known` sends this
    // same reading instead; `last_pushed` above is already updated either
    // way, so a transient failure here doesn't get retried redundantly
    // next tick with the same unchanged reading.
    let _ = crate::ble::set_device_settings(
        &ble_state,
        crate::ble::cbor::DeviceSettingsWrite {
            weather_condition: Some(condition),
            temperature: Some(temperature),
            temperature_unit: Some(unit),
            is_night: Some(is_night as u8),
            intensity: Some(intensity),
            ..Default::default()
        },
    )
    .await;
    Ok(())
}

/// The raw `DeviceSettings.weather_condition` byte (3=Rain, 4=Thunderstorm,
/// 5=Snow — see `Condition` above) from the last successfully pushed
/// reading, or `None` if nothing's been resolved yet this session.
/// `reminders.rs`'s end-of-day rain/snow reminder reads this rather than
/// fetching its own forecast — the existing ~15-min current-conditions poll
/// is already fresh enough for a same-day reminder.
pub async fn last_condition(state: &WeatherState) -> Option<u8> {
    state.last_pushed.lock().await.map(|r| r.condition)
}

/// Re-sends whatever was last successfully computed, with NO fresh fetch —
/// called from `start_post_sync_tasks` on every (re)connect (ble-protocol.md
/// §6.2/§6.4: "Weather... written together, on (re)connect and whenever the
/// poll... detects an actual change"). A no-op when nothing's been resolved
/// yet (e.g. very first launch, before `set_location`'s first fetch
/// completes) — the poll loop's own change-detected push covers that case
/// once a reading does arrive, whichever order location-resolution and the
/// BLE connection happen to complete in.
pub async fn push_last_known(app: &tauri::AppHandle, ble_state: &crate::ble::BleState) -> Result<(), String> {
    let state = app.state::<WeatherState>();
    let Some(reading) = *state.last_pushed.lock().await else {
        return Ok(());
    };
    crate::ble::set_device_settings(
        ble_state,
        crate::ble::cbor::DeviceSettingsWrite {
            weather_condition: Some(reading.condition),
            temperature: Some(reading.temperature),
            temperature_unit: Some(reading.unit),
            is_night: Some(reading.is_night as u8),
            intensity: Some(reading.intensity),
            ..Default::default()
        },
    )
    .await
}

/// Same ~15 min cadence as `calendar_import.rs`'s own poll interval —
/// consistent with the rest of ble-protocol.md §6.3's periodic-refresh
/// table, and no upstream data source here refreshes fast enough for
/// anything shorter to buy real freshness anyway.
const POLL_INTERVAL: std::time::Duration = std::time::Duration::from_secs(15 * 60);

/// Spawns the background task that keeps the weather reading fresh for the
/// lifetime of the app — called exactly once, from `lib.rs`'s `.setup()`,
/// mirroring `calendar_import::spawn_poll_task`'s identical reasoning
/// (including why `tauri::async_runtime::spawn` and not raw `tokio::spawn`
/// is required here: `.setup()` runs before Tauri binds a Tokio reactor to
/// the calling thread).
pub fn spawn_poll_task(app: tauri::AppHandle) {
    tauri::async_runtime::spawn(async move {
        loop {
            tokio::time::sleep(POLL_INTERVAL).await;
            if let Err(e) = refresh(&app).await {
                eprintln!("weather poll: {e}");
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wmo_codes_map_to_expected_conditions() {
        assert_eq!(map_wmo_code(0) as u8, Condition::Clear as u8);
        assert_eq!(map_wmo_code(2) as u8, Condition::PartlyCloudy as u8);
        assert_eq!(map_wmo_code(3) as u8, Condition::Cloudy as u8);
        assert_eq!(map_wmo_code(48) as u8, Condition::Fog as u8);
        assert_eq!(map_wmo_code(63) as u8, Condition::Rain as u8);
        assert_eq!(map_wmo_code(82) as u8, Condition::Rain as u8);
        assert_eq!(map_wmo_code(75) as u8, Condition::Snow as u8);
        assert_eq!(map_wmo_code(99) as u8, Condition::Thunderstorm as u8);
        // Unrecognized code falls back to Cloudy rather than panicking or
        // silently defaulting to Clear (which would misleadingly read as
        // "verified good weather" for data we don't actually understand).
        assert_eq!(map_wmo_code(12345) as u8, Condition::Cloudy as u8);
    }

    #[test]
    fn wmo_codes_map_to_expected_intensities() {
        // Rain family
        assert_eq!(map_wmo_intensity(51) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(53) as u8, Intensity::Moderate as u8);
        assert_eq!(map_wmo_intensity(55) as u8, Intensity::Heavy as u8);
        assert_eq!(map_wmo_intensity(56) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(57) as u8, Intensity::Heavy as u8);
        assert_eq!(map_wmo_intensity(61) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(63) as u8, Intensity::Moderate as u8);
        assert_eq!(map_wmo_intensity(65) as u8, Intensity::Heavy as u8);
        assert_eq!(map_wmo_intensity(66) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(67) as u8, Intensity::Heavy as u8);
        assert_eq!(map_wmo_intensity(80) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(81) as u8, Intensity::Moderate as u8);
        assert_eq!(map_wmo_intensity(82) as u8, Intensity::Heavy as u8);
        // Snow family
        assert_eq!(map_wmo_intensity(71) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(73) as u8, Intensity::Moderate as u8);
        assert_eq!(map_wmo_intensity(75) as u8, Intensity::Heavy as u8);
        assert_eq!(map_wmo_intensity(77) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(85) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(86) as u8, Intensity::Heavy as u8);
        // Thunderstorm
        assert_eq!(map_wmo_intensity(95) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(96) as u8, Intensity::Moderate as u8);
        assert_eq!(map_wmo_intensity(99) as u8, Intensity::Heavy as u8);
        // Fog
        assert_eq!(map_wmo_intensity(45) as u8, Intensity::Light as u8);
        assert_eq!(map_wmo_intensity(48) as u8, Intensity::Heavy as u8);
        // Clear/cloudy family and unrecognized codes => None
        assert_eq!(map_wmo_intensity(0) as u8, Intensity::None as u8);
        assert_eq!(map_wmo_intensity(1) as u8, Intensity::None as u8);
        assert_eq!(map_wmo_intensity(2) as u8, Intensity::None as u8);
        assert_eq!(map_wmo_intensity(3) as u8, Intensity::None as u8);
        assert_eq!(map_wmo_intensity(12345) as u8, Intensity::None as u8);
    }

    #[test]
    fn fahrenheit_country_list_covers_us_and_excludes_common_celsius_countries() {
        assert!(FAHRENHEIT_COUNTRIES.contains(&"US"));
        assert!(!FAHRENHEIT_COUNTRIES.contains(&"CA"));
        assert!(!FAHRENHEIT_COUNTRIES.contains(&"GB"));
        assert!(!FAHRENHEIT_COUNTRIES.contains(&"VN"));
    }
}
