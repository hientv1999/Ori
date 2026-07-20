// Local holiday support — Lunar Holiday List (char 0013, ble-protocol.md
// §3/§4) + Device Settings "g"/"j" (holiday_country/holiday_region, §4/§6.4).
//
// Firmware computes holidays for 8 countries (US/VN/CA/GB/AU/ES/MX/FR) from
// compact rule tables (Fixed-date, Nth-weekday-of-month, Last-weekday-of-
// month, Weekday-before/-on-or-after a date, Easter-offset) plus a regional
// override layer (holiday_region) for well-documented subdivisions — zero
// BLE dependency, see firmware/src/holiday_data.cpp. The one holiday no rule
// table can express is Vietnamese Tet (lunar New Year), which needs a real
// lunisolar calendar computation, not a closed-form date rule. Orion computes
// the full 1970-2100 table once per app session (the range firmware's u16
// epoch-day NVS cache can hold) and pushes it to Ori as a flat list of
// epoch-days over the Lunar Holiday List characteristic — no BEGIN/END
// staging, same simple treatment as Media Album Art (ble-protocol.md §5).
// Ori caches the list (and the holiday_country/holiday_region selection) in
// NVS, so once synced successfully one time, Tet renders correctly fully
// offline forever after — this is why the table is computed once per
// session and just resent, never recomputed per push.
//
// holiday_country/holiday_region are derived from the SAME location
// weather.rs's lookup already resolved (weather::country_code()/
// region_code()) — no second geolocation call for features that both just
// need "where is this PC." country_to_device_value(): 0=None 1=US 2=VN 3=CA
// 4=GB 5=AU 6=ES 7=MX 8=FR; any other/unresolved country sends 0 — firmware
// has no rule table for it, so no highlighting is the correct behavior, not
// a placeholder guess. region_to_device_value() maps per-country (see its
// own doc comment); France always resolves to region 0 (see below).
//
// Known, accepted limitation: this crate computes the Chinese lunisolar
// calendar, referenced to the UTC+8 meridian. Vietnamese Tet is defined
// against UTC+7 and can, in rare years, land a day earlier than Chinese New
// Year would. Not corrected here — a documented, accepted divergence, same
// spirit as ble-protocol.md's other "known limitation" call-outs.

use std::sync::OnceLock;

use chinese_lunisolar_calendar::{LunarDay, LunarMonth, LunisolarDate, LunisolarYear, SolarYear};
use chrono::NaiveDate;
use tauri::Manager;
use tokio::sync::Mutex;

const FIRST_YEAR: u16 = 1970;
const LAST_YEAR: u16 = 2100;

#[derive(Default)]
pub struct HolidayState {
    lunar_days: OnceLock<Vec<u16>>,
    country: Mutex<Option<u8>>,
    region: Mutex<Option<u8>>,
}

/// Computes every Lunar New Year (Tet) date from `FIRST_YEAR` to
/// `LAST_YEAR` inclusive, as epoch-days (days since 1970-01-01) — the same
/// representation firmware's `holiday_data::set_lunar_days()` NVS cache
/// stores. A year the crate can't resolve (only possible at the extreme
/// edges of its own supported range, 1901-02-19 to 2101-01-28 — well outside
/// FIRST_YEAR/LAST_YEAR) is silently skipped rather than aborting the whole
/// table.
fn compute_lunar_new_years() -> Vec<u16> {
    let epoch = NaiveDate::from_ymd_opt(1970, 1, 1).expect("valid date");
    let mut days = Vec::with_capacity((LAST_YEAR - FIRST_YEAR + 1) as usize);
    for year in FIRST_YEAR..=LAST_YEAR {
        let solar_year = SolarYear::from_u16(year);
        let Ok(lunisolar_year) = LunisolarYear::from_solar_year(solar_year) else { continue };
        let Ok(new_year) = LunisolarDate::from_lunisolar_year_lunar_month_day(
            lunisolar_year,
            LunarMonth::First,
            LunarDay::First,
        ) else {
            continue;
        };
        let naive = new_year.to_solar_date().to_naive_date();
        let epoch_day = (naive - epoch).num_days();
        if (0..=u16::MAX as i64).contains(&epoch_day) {
            days.push(epoch_day as u16);
        }
    }
    days
}

fn lunar_days(state: &HolidayState) -> &[u16] {
    state.lunar_days.get_or_init(compute_lunar_new_years)
}

/// `holiday_data::Country` on Ori's side (ble-protocol.md §4) — covers the
/// primary country for each of Orion's 4 supported UI languages (English:
/// US/CA/GB/AU, Vietnamese: VN, Spanish: ES/MX, French: CA/FR) plus Canada
/// twice over (English+French). `country_code` is the raw ISO 3166-1
/// alpha-2 code `weather::country_code()` resolved (e.g. from BigDataCloud's
/// reverse-geocode or ipapi.co) — note the UK's code is "GB", not "UK".
/// Anything else (including an unresolved location) sends 0/None — no rule
/// table exists for it, so no highlighting is correct, not a guess.
fn country_to_device_value(country_code: Option<&str>) -> u8 {
    match country_code {
        Some("US") => 1,
        Some("VN") => 2,
        Some("CA") => 3,
        Some("GB") => 4,
        Some("AU") => 5,
        Some("ES") => 6,
        Some("MX") => 7,
        Some("FR") => 8,
        _ => 0,
    }
}

/// `holiday_data`'s per-country region code (holiday_data.h's own table) —
/// dispatches on the already-resolved `country_value` (from
/// `country_to_device_value()` above) since a bare region code like "BC" or
/// "AB" only means something once you know which country it's in.
/// `region_code` is the bare (country-prefix-stripped) ISO 3166-2 code
/// `weather::region_code()` resolved. Country/region combinations with no
/// well-documented regional rules (Mexico, Vietnam, and every subdivision of
/// France — Alsace-Moselle is a *département*-level carve-out, finer than
/// the *region*-level subdivision code this resolves — pc-app.md) always
/// send 0, same "don't guess" reasoning as an unsupported country.
fn region_to_device_value(country_value: u8, region_code: Option<&str>) -> u8 {
    let Some(region_code) = region_code else { return 0 };
    match country_value {
        3 => match region_code { // CA
            "BC" => 1, "ON" => 2, "AB" => 3, "SK" => 4, "MB" => 5, "NB" => 6,
            "NS" => 7, "PE" => 8, "NL" => 9, "QC" => 10, "YT" => 11, "NT" => 12, "NU" => 13,
            _ => 0,
        },
        4 => match region_code { // GB
            "SCT" => 1, "NIR" => 2,
            _ => 0,
        },
        5 => match region_code { // AU
            "NSW" => 1, "VIC" => 2, "QLD" => 3, "WA" => 4, "SA" => 5, "TAS" => 6, "ACT" => 7, "NT" => 8,
            _ => 0,
        },
        6 => match region_code { // ES
            "AN" => 1, "AR" => 2, "AS" => 3, "IB" => 4, "CN" => 5, "CB" => 6,
            "CL" => 7, "CM" => 8, "CT" => 9, "VC" => 10, "EX" => 11, "GA" => 12,
            "MD" => 13, "MC" => 14, "PV" => 16, "RI" => 17, "CE" => 18, "ML" => 19,
            _ => 0,
        },
        1 => match region_code { // US
            "MA" => 1, "ME" => 2, "TX" => 3, "AK" => 4, "HI" => 5,
            _ => 0,
        },
        _ => 0, // VN, MX, FR (Alsace-Moselle needs department-level detection — see doc comment above), unrecognized country
    }
}

/// Eagerly computes the lunar table so it's ready before the first BLE
/// (re)connect might need it — called once from `lib.rs`'s `.setup()`. Cheap
/// (a few hundred iterations of plain date arithmetic), so doing this
/// up-front costs nothing measurable at startup.
pub fn init(app: &tauri::AppHandle) {
    let state = app.state::<HolidayState>();
    lunar_days(&state);
}

/// Derives `holiday_country` from the SAME location weather.rs just
/// resolved and pushes both it and the (already-computed) lunar table to
/// Ori. Called from the tail of `weather::set_location()`, the moment
/// geolocation/IP-fallback resolution completes — best-effort, matching
/// every other push in this module.
pub async fn resolve_and_push(app: &tauri::AppHandle) {
    let state = app.state::<HolidayState>();
    let country_code = crate::weather::country_code(app).await;
    let country_value = country_to_device_value(country_code.as_deref());
    *state.country.lock().await = Some(country_value);

    let region_code = crate::weather::region_code(app).await;
    let region_value = region_to_device_value(country_value, region_code.as_deref());
    *state.region.lock().await = Some(region_value);

    let ble_state = app.state::<crate::ble::BleState>();
    let _ = push_last_known(app, &ble_state).await;
}

/// Re-sends the lunar table + holiday_country with no recomputation — called
/// from `start_post_sync_tasks` on every (re)connect, mirroring
/// `weather::push_last_known`'s identical reasoning. The lunar-table push
/// always has data once `init()` has run (it doesn't depend on location);
/// the country push is a no-op until `resolve_and_push` has run at least
/// once this session.
///
/// The two pushes are independent and each best-effort — previously chained
/// with `?`, which meant a failure in the Lunar Holiday List write (e.g. a
/// transient chunked-write failure, or the Device Settings characteristic
/// being momentarily contended by another concurrent push) silently skipped
/// the unrelated country/region push too, even though either one succeeding
/// or failing has nothing to do with the other.
pub async fn push_last_known(app: &tauri::AppHandle, ble_state: &crate::ble::BleState) -> Result<(), String> {
    let state = app.state::<HolidayState>();
    let _ = crate::ble::push_lunar_holidays(ble_state, lunar_days(&state)).await;

    let country_value = *state.country.lock().await;
    let region_value = *state.region.lock().await;
    if country_value.is_some() || region_value.is_some() {
        let _ = crate::ble::set_device_settings(
            ble_state,
            crate::ble::cbor::DeviceSettingsWrite {
                holiday_country: country_value,
                holiday_region: region_value,
                ..Default::default()
            },
        )
        .await;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_tet_dates_match_reference_years() {
        let days = compute_lunar_new_years();
        let epoch = NaiveDate::from_ymd_opt(1970, 1, 1).unwrap();
        let has = |y: i32, m: u32, d: u32| {
            let target = (NaiveDate::from_ymd_opt(y, m, d).unwrap() - epoch).num_days() as u16;
            days.contains(&target)
        };
        // Well-known reference Tet dates.
        assert!(has(2024, 2, 10));
        assert!(has(2025, 1, 29));
        assert!(has(2026, 2, 17));
    }

    #[test]
    fn country_mapping() {
        assert_eq!(country_to_device_value(Some("US")), 1);
        assert_eq!(country_to_device_value(Some("VN")), 2);
        assert_eq!(country_to_device_value(Some("CA")), 3);
        assert_eq!(country_to_device_value(Some("GB")), 4);
        assert_eq!(country_to_device_value(Some("AU")), 5);
        assert_eq!(country_to_device_value(Some("ES")), 6);
        assert_eq!(country_to_device_value(Some("MX")), 7);
        assert_eq!(country_to_device_value(Some("FR")), 8);
        // Not a supported country, and an unresolved location — both None.
        assert_eq!(country_to_device_value(Some("DE")), 0);
        assert_eq!(country_to_device_value(Some("UK")), 0); // ISO code is "GB", not "UK"
        assert_eq!(country_to_device_value(None), 0);
    }

    #[test]
    fn region_mapping() {
        // Canada (country_value 3).
        assert_eq!(region_to_device_value(3, Some("BC")), 1);
        assert_eq!(region_to_device_value(3, Some("QC")), 10);
        assert_eq!(region_to_device_value(3, Some("NU")), 13);
        // UK (4).
        assert_eq!(region_to_device_value(4, Some("SCT")), 1);
        assert_eq!(region_to_device_value(4, Some("NIR")), 2);
        assert_eq!(region_to_device_value(4, Some("ENG")), 0); // England = the national default
        // Australia (5).
        assert_eq!(region_to_device_value(5, Some("QLD")), 3);
        assert_eq!(region_to_device_value(5, Some("WA")), 4);
        // Spain (6).
        assert_eq!(region_to_device_value(6, Some("CT")), 9); // Cataluña
        assert_eq!(region_to_device_value(6, Some("NC")), 0); // Navarra — no-op, matches firmware
        // US (1).
        assert_eq!(region_to_device_value(1, Some("MA")), 1);
        assert_eq!(region_to_device_value(1, Some("CA")), 0); // California isn't one of the 5 modeled states
        // No regional table at all: VN(2), MX(7), FR(8).
        assert_eq!(region_to_device_value(2, Some("BC")), 0);
        assert_eq!(region_to_device_value(7, Some("JAL")), 0);
        assert_eq!(region_to_device_value(8, Some("GES")), 0); // Grand Est — Alsace-Moselle detection gap
        // A region code that doesn't match anything under its country, and
        // an unresolved region — both 0.
        assert_eq!(region_to_device_value(3, Some("XX")), 0);
        assert_eq!(region_to_device_value(3, None), 0);
    }
}
