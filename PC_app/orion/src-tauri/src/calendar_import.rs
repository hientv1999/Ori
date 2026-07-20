// Calendar Source (pc-app.md) — Outlook sharing-invitation XML import +
// ICS feed polling. Deliberately NOT Microsoft Graph/OAuth: see pc-app.md's
// "Calendar data" section for why. Two independent pieces live here:
//
//   1. extract_xml_fields() — one-time, local, no network: pulls
//      name/email/ICalUrl out of a sharing_metadata.xml the user already has
//      on disk (tools/parse_sharing_metadata.py is the Python reference this
//      mirrors).
//   2. fetch_meetings() — repeated, over the network: GETs the ICS feed the
//      import found and turns "today's occurrences" into CachedMeeting
//      values ready to hand to central.rs's run_sync()
//      (tools/test_ics_calendar.py is the Python reference this mirrors,
//      including the recurrence-expansion requirement).
//
// Known, accepted limitation (pc-app.md): published Exchange ICS feeds have
// no refresh SLA — real-world lag is commonly hours, occasionally a full
// day. Nothing in this file can fix that; it's inherent to the data source.

use icalendar::{Calendar, CalendarComponent, Component, EventLike};
use std::collections::HashMap;

/// Fields pulled from a sharing_metadata.xml — see
/// tools/parse_sharing_metadata.py for the reference implementation this
/// mirrors field-for-field (Initiator/Name, Initiator/SmtpAddress,
/// Invitation/Providers/Provider/ICalUrl). Everything else in the file
/// (EntryId, BrowseUrl, TargetRecipients, Provider Type) is read only to
/// confirm the document parses, never stored.
pub struct XmlFields {
    pub name: Option<String>,
    pub email: Option<String>,
    pub ical_url: Option<String>,
}

/// One meeting occurrence ready for the wire (ble-protocol.md §4's
/// `Meeting`), owned rather than borrowed like `cbor::Meeting<'a>` since it
/// has to survive being stashed in `BleState::cached_meetings` between the
/// poll task that builds it and whatever later `run_sync` call reads it.
/// `organizer` isn't a field here at all — see fetch_meetings()'s doc
/// comment for why there's nothing to put in it. `PartialEq` backs
/// `BleState::replace_cached_meetings`'s change detection; `Clone` lets
/// `refresh()` hand the same fetched list to both the cache and a
/// best-effort live push.
#[derive(Clone, PartialEq)]
pub struct CachedMeeting {
    pub uid: String,
    pub start: u64,
    pub end: u64,
    pub title: String,
    pub location: String,
}

// ── XML import ──────────────────────────────────────────────────────────

/// Reads and parses a sharing_metadata.xml file already on disk (path
/// chosen via the native file picker, commands.rs's import_calendar_xml).
/// Namespace-agnostic by design (`roxmltree`'s `has_tag_name()` matches only
/// the local element name) — the document lives entirely in one default
/// `xmlns`, not per-element prefixes, so this doesn't need to know or care
/// what that namespace URI actually is.
pub fn extract_xml_fields(xml: &str) -> Result<XmlFields, String> {
    let doc = roxmltree::Document::parse(xml).map_err(|e| format!("not valid XML: {e}"))?;

    let find_text = |tag: &str| -> Option<String> {
        doc.root()
            .descendants()
            .find(|n| n.has_tag_name(tag))
            .and_then(|n| n.text())
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
    };

    let name = find_text("Name");
    let email = find_text("SmtpAddress");
    // The schema allows more than one <Provider> (maxOccurs="unbounded");
    // in practice a sharing invitation has exactly one, so the first
    // ICalUrl found is the one we want — no need to disambiguate by the
    // Provider's own Type attribute for this use case.
    let ical_url = find_text("ICalUrl");

    if name.is_none() && email.is_none() && ical_url.is_none() {
        return Err("none of Name/SmtpAddress/ICalUrl were found in this file — is it really a sharing_metadata.xml?".into());
    }

    Ok(XmlFields { name, email, ical_url })
}

// ── ICS fetch + parse + recurrence expansion ────────────────────────────

/// Windows timezone display name → canonical IANA zone, from CLDR's
/// windowsZones.xml (`territory="001"` rows — the default zone per Windows
/// name; fetched 2026-07-16 from unicode-org/cldr). Exchange-published ICS
/// feeds stamp every timed event with the WINDOWS name ("Eastern Standard
/// Time"), which `chrono-tz` — the resolver behind both `icalendar`'s
/// `try_into_utc()` and `rrule`'s `Tz` — cannot parse, so without this
/// rewrite EVERY timed meeting in a real Exchange feed silently fails
/// `event_start_utc()` and gets dropped (found 2026-07-16: an import with 3
/// real meetings in the feed reported "no meetings today").
///
/// Sorted longest-name-first, and MUST stay that way: several names are
/// strict prefix extensions of others ("Central Standard Time (Mexico)" vs
/// "Central Standard Time"), and `normalize_windows_tzids` replaces in
/// table order — a shorter prefix replaced first would corrupt the longer
/// name mid-string.
const WINDOWS_TZ_TO_IANA: &[(&str, &str)] = &[
    ("Central Brazilian Standard Time", "America/Cuiaba"),
    ("Mountain Standard Time (Mexico)", "America/Mazatlan"),
    ("W. Central Africa Standard Time", "Africa/Lagos"),
    ("Central European Standard Time", "Europe/Warsaw"),
    ("Central Standard Time (Mexico)", "America/Mexico_City"),
    ("E. South America Standard Time", "America/Sao_Paulo"),
    ("Eastern Standard Time (Mexico)", "America/Cancun"),
    ("Pacific Standard Time (Mexico)", "America/Tijuana"),
    ("Turks And Caicos Standard Time", "America/Grand_Turk"),
    ("Central America Standard Time", "America/Guatemala"),
    ("Central Pacific Standard Time", "Pacific/Guadalcanal"),
    ("Chatham Islands Standard Time", "Pacific/Chatham"),
    ("N. Central Asia Standard Time", "Asia/Novosibirsk"),
    ("North Asia East Standard Time", "Asia/Irkutsk"),
    ("Aus Central W. Standard Time", "Australia/Eucla"),
    ("Canada Central Standard Time", "America/Regina"),
    ("Cen. Australia Standard Time", "Australia/Adelaide"),
    ("Central Europe Standard Time", "Europe/Budapest"),
    ("Easter Island Standard Time", "Pacific/Easter"),
    ("Bougainville Standard Time", "Pacific/Bougainville"),
    ("Central Asia Standard Time", "Asia/Bishkek"),
    ("E. Australia Standard Time", "Australia/Brisbane"),
    ("Ekaterinburg Standard Time", "Asia/Yekaterinburg"),
    ("Line Islands Standard Time", "Pacific/Kiritimati"),
    ("Newfoundland Standard Time", "America/St_Johns"),
    ("Saint Pierre Standard Time", "America/Miquelon"),
    ("South Africa Standard Time", "Africa/Johannesburg"),
    ("W. Australia Standard Time", "Australia/Perth"),
    ("West Pacific Standard Time", "Pacific/Port_Moresby"),
    ("AUS Central Standard Time", "Australia/Darwin"),
    ("AUS Eastern Standard Time", "Australia/Sydney"),
    ("Afghanistan Standard Time", "Asia/Kabul"),
    ("Kaliningrad Standard Time", "Europe/Kaliningrad"),
    ("Middle East Standard Time", "Asia/Beirut"),
    ("New Zealand Standard Time", "Pacific/Auckland"),
    ("North Korea Standard Time", "Asia/Pyongyang"),
    ("South Sudan Standard Time", "Africa/Juba"),
    ("Transbaikal Standard Time", "Asia/Chita"),
    ("US Mountain Standard Time", "America/Phoenix"),
    ("Ulaanbaatar Standard Time", "Asia/Ulaanbaatar"),
    ("Vladivostok Standard Time", "Asia/Vladivostok"),
    ("W. Mongolia Standard Time", "Asia/Hovd"),
    ("Azerbaijan Standard Time", "Asia/Baku"),
    ("Bangladesh Standard Time", "Asia/Dhaka"),
    ("Cape Verde Standard Time", "Atlantic/Cape_Verde"),
    ("Magallanes Standard Time", "America/Punta_Arenas"),
    ("Montevideo Standard Time", "America/Montevideo"),
    ("North Asia Standard Time", "Asia/Krasnoyarsk"),
    ("Pacific SA Standard Time", "America/Santiago"),
    ("SA Eastern Standard Time", "America/Cayenne"),
    ("SA Pacific Standard Time", "America/Bogota"),
    ("SA Western Standard Time", "America/La_Paz"),
    ("US Eastern Standard Time", "America/Indianapolis"),
    ("Argentina Standard Time", "America/Buenos_Aires"),
    ("Astrakhan Standard Time", "Europe/Astrakhan"),
    ("E. Africa Standard Time", "Africa/Nairobi"),
    ("E. Europe Standard Time", "Europe/Chisinau"),
    ("Greenland Standard Time", "America/Godthab"),
    ("Greenwich Standard Time", "Atlantic/Reykjavik"),
    ("Lord Howe Standard Time", "Australia/Lord_Howe"),
    ("Marquesas Standard Time", "Pacific/Marquesas"),
    ("Mauritius Standard Time", "Indian/Mauritius"),
    ("Qyzylorda Standard Time", "Asia/Qyzylorda"),
    ("Singapore Standard Time", "Asia/Singapore"),
    ("Sri Lanka Standard Time", "Asia/Colombo"),
    ("Tocantins Standard Time", "America/Araguaina"),
    ("Venezuela Standard Time", "America/Caracas"),
    ("Volgograd Standard Time", "Europe/Volgograd"),
    ("W. Europe Standard Time", "Europe/Berlin"),
    ("West Asia Standard Time", "Asia/Tashkent"),
    ("West Bank Standard Time", "Asia/Hebron"),
    ("Aleutian Standard Time", "America/Adak"),
    ("Atlantic Standard Time", "America/Halifax"),
    ("Caucasus Standard Time", "Asia/Yerevan"),
    ("Dateline Standard Time", "Etc/GMT+12"),
    ("Georgian Standard Time", "Asia/Tbilisi"),
    ("Hawaiian Standard Time", "Pacific/Honolulu"),
    ("Mountain Standard Time", "America/Denver"),
    ("Pakistan Standard Time", "Asia/Karachi"),
    ("Paraguay Standard Time", "America/Asuncion"),
    ("Sakhalin Standard Time", "Asia/Sakhalin"),
    ("Sao Tome Standard Time", "Africa/Sao_Tome"),
    ("Tasmania Standard Time", "Australia/Hobart"),
    ("Alaskan Standard Time", "America/Anchorage"),
    ("Arabian Standard Time", "Asia/Dubai"),
    ("Belarus Standard Time", "Europe/Minsk"),
    ("Central Standard Time", "America/Chicago"),
    ("Eastern Standard Time", "America/New_York"),
    ("Magadan Standard Time", "Asia/Magadan"),
    ("Morocco Standard Time", "Africa/Casablanca"),
    ("Myanmar Standard Time", "Asia/Rangoon"),
    ("Namibia Standard Time", "Africa/Windhoek"),
    ("Norfolk Standard Time", "Pacific/Norfolk"),
    ("Pacific Standard Time", "America/Los_Angeles"),
    ("Romance Standard Time", "Europe/Paris"),
    ("Russian Standard Time", "Europe/Moscow"),
    ("SE Asia Standard Time", "Asia/Bangkok"),
    ("Saratov Standard Time", "Europe/Saratov"),
    ("Yakutsk Standard Time", "Asia/Yakutsk"),
    ("Arabic Standard Time", "Asia/Baghdad"),
    ("Azores Standard Time", "Atlantic/Azores"),
    ("Israel Standard Time", "Asia/Jerusalem"),
    ("Jordan Standard Time", "Asia/Amman"),
    ("Taipei Standard Time", "Asia/Taipei"),
    ("Turkey Standard Time", "Europe/Istanbul"),
    ("Altai Standard Time", "Asia/Barnaul"),
    ("Bahia Standard Time", "America/Bahia"),
    ("China Standard Time", "Asia/Shanghai"),
    ("Egypt Standard Time", "Africa/Cairo"),
    ("Haiti Standard Time", "America/Port-au-Prince"),
    ("India Standard Time", "Asia/Calcutta"),
    ("Korea Standard Time", "Asia/Seoul"),
    ("Libya Standard Time", "Africa/Tripoli"),
    ("Nepal Standard Time", "Asia/Katmandu"),
    ("Russia Time Zone 10", "Asia/Srednekolymsk"),
    ("Russia Time Zone 11", "Asia/Kamchatka"),
    ("Samoa Standard Time", "Pacific/Apia"),
    ("Sudan Standard Time", "Africa/Khartoum"),
    ("Syria Standard Time", "Asia/Damascus"),
    ("Tokyo Standard Time", "Asia/Tokyo"),
    ("Tomsk Standard Time", "Asia/Tomsk"),
    ("Tonga Standard Time", "Pacific/Tongatapu"),
    ("Yukon Standard Time", "America/Whitehorse"),
    ("Arab Standard Time", "Asia/Riyadh"),
    ("Cuba Standard Time", "America/Havana"),
    ("Fiji Standard Time", "Pacific/Fiji"),
    ("Iran Standard Time", "Asia/Tehran"),
    ("Omsk Standard Time", "Asia/Omsk"),
    ("Russia Time Zone 3", "Europe/Samara"),
    ("FLE Standard Time", "Europe/Kiev"),
    ("GMT Standard Time", "Europe/London"),
    ("GTB Standard Time", "Europe/Bucharest"),
    ("UTC+12", "Etc/GMT-12"),
    ("UTC+13", "Etc/GMT-13"),
    ("UTC-02", "Etc/GMT+2"),
    ("UTC-08", "Etc/GMT+8"),
    ("UTC-09", "Etc/GMT+9"),
    ("UTC-11", "Etc/GMT+11"),
    ("UTC", "Etc/UTC"),
];

/// Rewrites Windows timezone names to IANA in the raw ICS text, BEFORE any
/// parsing — one rewrite fixes both downstream consumers at once
/// (`icalendar`'s DTSTART/DTEND/RECURRENCE-ID resolution and `rrule`'s own
/// DTSTART parse inside `get_recurrence()`), instead of patching each
/// resolver separately. Handles all three forms Exchange emits: the
/// property parameter (`DTSTART;TZID=Eastern Standard Time:...`), its
/// quoted variant, and the VTIMEZONE component's own `TZID:...` property
/// (that last one is cosmetic — nothing reads VTIMEZONE blocks — but
/// leaving it inconsistent invites confusion in logged/dumped feeds).
/// Unknown TZIDs pass through unchanged and their events get skipped (and
/// counted) downstream rather than failing the whole parse.
fn normalize_windows_tzids(ics: &str) -> String {
    let mut out = ics.to_string();
    for (win, iana) in WINDOWS_TZ_TO_IANA {
        if !out.contains(win) {
            continue;
        }
        out = out
            .replace(&format!("TZID=\"{win}\""), &format!("TZID={iana}"))
            .replace(&format!("TZID={win}"), &format!("TZID={iana}"))
            .replace(&format!("TZID:{win}"), &format!("TZID:{iana}"));
    }
    out
}

/// Fetches the ICS feed and returns every occurrence falling within
/// [`day_start_epoch`, `day_end_epoch`) — both plain Unix epoch seconds, the
/// same boundary `central.rs`'s existing `local_midnight_epoch()` already
/// computes for `MeetingList.d`, reused here rather than re-deriving "local
/// today" a second way. Recurring events (RRULE) are expanded, not read as
/// one raw block per series — see this module's own doc comment.
///
/// `organizer` is never populated (ble-protocol.md's Meeting.o is always
/// sent as ""): published/shared Exchange ICS feeds strip ORGANIZER and
/// ATTENDEE entirely. This isn't a parsing gap to fix — there is no
/// organizer in the feed to read.
pub async fn fetch_meetings(
    ical_url: &str,
    day_start_epoch: u64,
    day_end_epoch: u64,
) -> Result<Vec<CachedMeeting>, String> {
    let resp = reqwest::get(ical_url).await.map_err(|e| format!("fetching ICS feed: {e}"))?;
    if !resp.status().is_success() {
        return Err(format!("ICS feed returned HTTP {}", resp.status()));
    }
    let text = resp.text().await.map_err(|e| format!("reading ICS feed body: {e}"))?;
    parse_meetings(&text, day_start_epoch, day_end_epoch)
}

/// The parse/filter/expand half of `fetch_meetings`, split from the network
/// half so the whole timezone/recurrence pipeline is testable against
/// inline ICS fixtures (see this file's tests) — the Windows-TZID bug this
/// split was made for is exactly the kind of thing a network-coupled
/// function hides.
fn parse_meetings(
    ics_text: &str,
    day_start_epoch: u64,
    day_end_epoch: u64,
) -> Result<Vec<CachedMeeting>, String> {
    let text = normalize_windows_tzids(ics_text);

    let calendar: Calendar = text
        .parse()
        .map_err(|e| format!("parsing ICS feed: {e}"))?;

    // RFC 5545: a modified single occurrence of a recurring series is a
    // SEPARATE VEVENT sharing the master's UID, carrying its own
    // RECURRENCE-ID (which original occurrence it replaces) plus its own
    // DTSTART/SUMMARY/etc. Neither `icalendar` nor `rrule` merges these —
    // group by UID first so overrides can supersede the master's expanded
    // occurrence instead of both showing up.
    let mut masters: HashMap<String, &icalendar::Event> = HashMap::new();
    let mut overrides: Vec<&icalendar::Event> = Vec::new();

    for component in calendar.components.iter() {
        let CalendarComponent::Event(event) = component else { continue };
        let Some(uid) = event.get_uid() else { continue };
        if event.property_value("RECURRENCE-ID").is_some() {
            overrides.push(event);
        } else {
            masters.insert(uid.to_string(), event);
        }
    }

    let day_start = chrono::DateTime::<chrono::Utc>::from_timestamp(day_start_epoch as i64, 0)
        .ok_or_else(|| "invalid day_start_epoch".to_string())?;
    let day_end = chrono::DateTime::<chrono::Utc>::from_timestamp(day_end_epoch as i64, 0)
        .ok_or_else(|| "invalid day_end_epoch".to_string())?;

    let mut out = Vec::new();
    let mut superseded: std::collections::HashSet<(String, i64)> = std::collections::HashSet::new();
    // Events whose start/end couldn't be resolved to a UTC instant — in
    // practice an unrecognized TZID that normalize_windows_tzids() didn't
    // cover. Counted and logged rather than silently dropped: this exact
    // silent-drop is how the Windows-TZID bug went unnoticed.
    let mut skipped = 0usize;

    // Overrides first, so their (uid, original-occurrence-start) pairs are
    // known before the master series below gets expanded and needs to skip
    // whichever occurrence each override replaces.
    for ev in &overrides {
        if is_cancelled(ev) {
            continue;
        }
        let (start, end) = match (event_start_utc(ev), event_end_utc(ev)) {
            (Some(s), Some(e)) => (s, e),
            _ => {
                skipped += 1;
                continue;
            }
        };
        if let Some(uid) = ev.get_uid() {
            if let Some(recurrence_id) = event_recurrence_id_utc(ev) {
                superseded.insert((uid.to_string(), recurrence_id.timestamp()));
            }
        }
        if start < day_end && end > day_start {
            out.push(CachedMeeting {
                uid: format!("{}-{}", ev.get_uid().unwrap_or(""), start.timestamp()),
                start: start.timestamp().max(0) as u64,
                end: end.timestamp().max(0) as u64,
                title: truncate_utf8(ev.get_summary().unwrap_or(""), 128),
                location: truncate_utf8(ev.get_location().unwrap_or(""), 64),
            });
        }
    }

    for (uid, ev) in &masters {
        if is_cancelled(ev) {
            continue;
        }
        let Some(dtstart) = event_start_utc(ev) else {
            skipped += 1;
            continue;
        };
        let duration = event_end_utc(ev).map(|e| e - dtstart).unwrap_or_else(|| chrono::Duration::hours(1));

        // get_recurrence() returns Result, not Option — Err covers both "no
        // RRULE on this event" and "the RRULE/DTSTART combination is
        // malformed"; either way, fall back to treating it as a single
        // non-recurring event rather than dropping it entirely.
        match ev.get_recurrence() {
            Ok(rrule_set) => {
                // RRuleSet::after()/before() take rrule's own Tz-parameterized
                // DateTime, not plain chrono::DateTime<Utc> — convert via
                // with_timezone rather than changing day_start/day_end's own
                // type, since those two are also compared directly against
                // plain Utc DateTimes elsewhere in this function (the
                // non-recurring and override branches).
                let rrule_start = day_start.with_timezone(&icalendar::rrule::Tz::UTC);
                let rrule_end = day_end.with_timezone(&icalendar::rrule::Tz::UTC);
                let result = rrule_set.after(rrule_start).before(rrule_end).all(366);
                for occ_start in result.dates {
                    if superseded.contains(&(uid.clone(), occ_start.timestamp())) {
                        continue; // this occurrence was replaced by an override handled above
                    }
                    let occ_start_utc = occ_start.with_timezone(&chrono::Utc);
                    let occ_end_utc = occ_start_utc + duration;
                    out.push(CachedMeeting {
                        uid: format!("{uid}-{}", occ_start_utc.timestamp()),
                        start: occ_start_utc.timestamp().max(0) as u64,
                        end: occ_end_utc.timestamp().max(0) as u64,
                        title: truncate_utf8(ev.get_summary().unwrap_or(""), 128),
                        location: truncate_utf8(ev.get_location().unwrap_or(""), 64),
                    });
                }
            }
            Err(_) => {
                // Non-recurring (or unparseable RRULE) — check its own
                // DTSTART/DTEND directly.
                let end = dtstart + duration;
                if dtstart < day_end && end > day_start {
                    out.push(CachedMeeting {
                        uid: uid.clone(),
                        start: dtstart.timestamp().max(0) as u64,
                        end: end.timestamp().max(0) as u64,
                        title: truncate_utf8(ev.get_summary().unwrap_or(""), 128),
                        location: truncate_utf8(ev.get_location().unwrap_or(""), 64),
                    });
                }
            }
        }
    }

    if skipped > 0 {
        eprintln!("calendar: skipped {skipped} event(s) with unresolvable start/end (unrecognized TZID?)");
    }

    out.sort_by_key(|m| m.start);
    Ok(out)
}

fn is_cancelled(ev: &icalendar::Event) -> bool {
    matches!(ev.get_status(), Some(icalendar::EventStatus::Cancelled))
}

fn event_start_utc(ev: &icalendar::Event) -> Option<chrono::DateTime<chrono::Utc>> {
    ev.get_start().and_then(date_perhaps_time_to_utc)
}

fn event_end_utc(ev: &icalendar::Event) -> Option<chrono::DateTime<chrono::Utc>> {
    ev.get_end().and_then(date_perhaps_time_to_utc)
}

fn event_recurrence_id_utc(ev: &icalendar::Event) -> Option<chrono::DateTime<chrono::Utc>> {
    ev.get_recurrence_id().and_then(date_perhaps_time_to_utc)
}

/// `icalendar`'s DTSTART/DTEND/RECURRENCE-ID accessors return
/// `DatePerhapsTime` (an event can be all-day, date-only) — normalize both
/// variants to a UTC instant. An all-day DATE value has no time-of-day or
/// zone in the ICS itself; treating it as UTC midnight is a reasonable
/// approximation for "does this fall on today's date," consistent with how
/// this feature only needs day-granularity filtering, not minute-precision
/// scheduling for all-day entries.
fn date_perhaps_time_to_utc(dpt: icalendar::DatePerhapsTime) -> Option<chrono::DateTime<chrono::Utc>> {
    use chrono::TimeZone;
    match dpt {
        icalendar::DatePerhapsTime::DateTime(cdt) => cdt.try_into_utc(),
        icalendar::DatePerhapsTime::Date(d) => {
            chrono::Utc.from_local_datetime(&d.and_hms_opt(0, 0, 0)?).single()
        }
    }
}

/// Truncates to at most `max_bytes` UTF-8 bytes on a char boundary — the
/// same truncation ble-protocol.md §10 says firmware does anyway if this is
/// skipped, done here so a too-long summary/location doesn't rely on that
/// fallback.
fn truncate_utf8(s: &str, max_bytes: usize) -> String {
    if s.len() <= max_bytes {
        return s.to_string();
    }
    let mut end = max_bytes;
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_string()
}

// ── Background poll ─────────────────────────────────────────────────────

/// pc-app.md's "~15 min" cadence — reused from ble-protocol.md §6.3's own
/// meeting-refresh interval since polling faster doesn't buy more freshness
/// against Exchange's own publish lag (pc-app.md's staleness caveat: real-
/// world lag is commonly hours regardless of how often this asks).
const POLL_INTERVAL: std::time::Duration = std::time::Duration::from_secs(15 * 60);

/// One fetch-and-cache cycle: reads whatever `calendar_ics_url` is currently
/// persisted (re-read fresh every call, not cached in the caller — so a
/// re-import or a `clear_calendar_import` between ticks is picked up on the
/// very next one with no extra plumbing), fetches today's occurrences, and
/// hands them to `BleState::replace_cached_meetings` for `run_sync` to read
/// on its next call. Runs independent of whether Ori is currently connected
/// — this cache update itself needs no live link.
///
/// If the fetch actually changed what was cached, this ALSO best-effort
/// pushes the update to Ori right away over `push_meetings` — pc-app.md's
/// "live resync whenever calendar data changes": a reconnect always resends
/// the current cache regardless (`run_sync` reads `cached_meetings` fresh
/// every session, so a change that arrives before Ori reconnects is simply
/// included in that sync), but Ori might already be connected and stay
/// connected for a long time, so *changes discovered while already
/// connected* need their own push rather than waiting for the next
/// disconnect/reconnect cycle. `push_meetings` no-ops cleanly (returns Err,
/// swallowed here) when Ori isn't reachable right now — the cache is still
/// updated either way, so the next reconnect picks it up regardless, same
/// "persist first, push is best-effort" pattern `save_profile` uses.
///
/// Returns the number of meetings cached — `Ok(0)` both when nothing's
/// configured yet (not an error: nothing to fetch) and when the feed
/// genuinely has nothing today. Callers that want to tell those two apart
/// should check `store::SavedState::calendar_ics_url` themselves first, as
/// `import_calendar_xml` does.
///
/// Emits `network-health` (pc-app.md's header warning icon) around the
/// actual network attempt — NOT the "nothing configured" early return above,
/// since that path never touches the network and has nothing to report.
/// Callers that need the raw fetch `Result` for their own purposes (rather
/// than this function's "returns 0 either way" collapsing) should read the
/// emitted event or, for a single one-off call, inspect the error text.
pub async fn refresh(app: &tauri::AppHandle) -> Result<usize, String> {
    use tauri::{Emitter, Manager};

    let Some(ical_url) = crate::store::load(app).await.and_then(|s| s.calendar_ics_url) else {
        return Ok(0);
    };

    let day_start = crate::ble::central::local_midnight_epoch();
    let day_end = day_start + 86_400;
    let result = fetch_meetings(&ical_url, day_start, day_end).await;
    let _ = app.emit("network-health", serde_json::json!({"source": "calendar", "ok": result.is_ok()}));
    let meetings = result?;
    let count = meetings.len();

    let state = app.state::<crate::ble::BleState>();
    // No `.clone()` needed — `meetings` moves straight into the cache, and
    // push_meetings (if the cache actually changed) reads it back out from
    // there itself rather than needing its own separate copy.
    let changed = state.replace_cached_meetings(meetings).await;
    if changed {
        let _ = crate::ble::central::push_meetings(&state).await;
    }
    Ok(count)
}

/// Spawns the long-running background task that keeps `cached_meetings`
/// fresh for the lifetime of the app. Called exactly once, from `lib.rs`'s
/// `.setup()`, regardless of whether a calendar source is configured yet —
/// `refresh()`'s own early return handles "nothing to poll" as a cheap no-op
/// each tick, so there's no need to gate spawning this on `calendar_ics_url`
/// already being set, and no need to ever spawn a second one later (e.g.
/// from `import_calendar_xml`): a fresh import is picked up by this same
/// already-running loop on its next tick, and `import_calendar_xml` does its
/// own one-off immediate `refresh()` call for instant feedback rather than
/// racing a second loop against this one.
///
/// Uses `tauri::async_runtime::spawn`, NOT raw `tokio::spawn` — `.setup()`
/// runs synchronously, before Tauri has bound a Tokio reactor to the calling
/// thread, so a bare `tokio::spawn` here panics ("there is no reactor
/// running, must be called from the context of a Tokio 1.x runtime"), unlike
/// every other `tokio::spawn` call site in this codebase, which runs from
/// inside an async `#[tauri::command]` handler already executing on Tauri's
/// managed runtime. `async_runtime::spawn` dispatches onto that runtime
/// regardless of the calling thread's own context.
pub fn spawn_poll_task(app: tauri::AppHandle) {
    tauri::async_runtime::spawn(async move {
        loop {
            if let Err(e) = refresh(&app).await {
                eprintln!("calendar poll: {e}");
            }
            tokio::time::sleep(POLL_INTERVAL).await;
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::TimeZone;

    fn ics(events: &str) -> String {
        format!("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//test//EN\r\n{events}END:VCALENDAR\r\n")
    }

    /// [day_start, day_end) covering all of 2026-07-20 (a Monday), UTC.
    fn monday_window() -> (u64, u64) {
        let start = chrono::Utc.with_ymd_and_hms(2026, 7, 20, 0, 0, 0).unwrap().timestamp() as u64;
        (start, start + 86_400)
    }

    #[test]
    fn windows_tzid_single_event_resolves() {
        // The exact shape Exchange publishes — a Windows display name as
        // TZID. 09:00 America/New_York in July = 13:00 UTC (EDT, UTC-4).
        let text = ics(concat!(
            "BEGIN:VEVENT\r\n",
            "UID:evt-1\r\n",
            "DTSTART;TZID=Eastern Standard Time:20260720T090000\r\n",
            "DTEND;TZID=Eastern Standard Time:20260720T093000\r\n",
            "SUMMARY:Sprint Start Meeting\r\n",
            "END:VEVENT\r\n",
        ));
        let (s, e) = monday_window();
        let meetings = parse_meetings(&text, s, e).unwrap();
        assert_eq!(meetings.len(), 1);
        assert_eq!(meetings[0].title, "Sprint Start Meeting");
        let expected = chrono::Utc.with_ymd_and_hms(2026, 7, 20, 13, 0, 0).unwrap().timestamp() as u64;
        assert_eq!(meetings[0].start, expected);
        assert_eq!(meetings[0].end, expected + 1800);
    }

    #[test]
    fn windows_tzid_recurring_event_expands() {
        // Weekly Monday series started two weeks before the window; 08:30
        // America/Los_Angeles in July = 15:30 UTC (PDT, UTC-7).
        let text = ics(concat!(
            "BEGIN:VEVENT\r\n",
            "UID:evt-2\r\n",
            "DTSTART;TZID=Pacific Standard Time:20260706T083000\r\n",
            "DTEND;TZID=Pacific Standard Time:20260706T084500\r\n",
            "RRULE:FREQ=WEEKLY;BYDAY=MO\r\n",
            "SUMMARY:Weekly Standup\r\n",
            "END:VEVENT\r\n",
        ));
        let (s, e) = monday_window();
        let meetings = parse_meetings(&text, s, e).unwrap();
        assert_eq!(meetings.len(), 1, "expected exactly the July 20 occurrence");
        let expected = chrono::Utc.with_ymd_and_hms(2026, 7, 20, 15, 30, 0).unwrap().timestamp() as u64;
        assert_eq!(meetings[0].start, expected);
        assert_eq!(meetings[0].end, expected + 900);
    }

    #[test]
    fn prefix_sharing_windows_names_do_not_corrupt() {
        // "Central Standard Time (Mexico)" contains "Central Standard Time"
        // — the longest-first table order must map it whole, never as
        // "America/Chicago (Mexico)".
        let out = normalize_windows_tzids("DTSTART;TZID=Central Standard Time (Mexico):20260720T090000\r\n");
        assert!(out.contains("TZID=America/Mexico_City:"), "got: {out}");
        assert!(!out.contains("Chicago"), "got: {out}");
    }

    #[test]
    fn unknown_tzid_skips_event_without_failing() {
        let text = ics(concat!(
            "BEGIN:VEVENT\r\n",
            "UID:evt-3\r\n",
            "DTSTART;TZID=Klingon Standard Time:20260720T090000\r\n",
            "DTEND;TZID=Klingon Standard Time:20260720T093000\r\n",
            "SUMMARY:Unmappable\r\n",
            "END:VEVENT\r\n",
        ));
        let (s, e) = monday_window();
        let meetings = parse_meetings(&text, s, e).unwrap();
        assert!(meetings.is_empty());
    }

    /// Live end-to-end check against the ICS URL stored in this machine's
    /// Orion state.json — network + a completed import required, so ignored
    /// by default. Run with:
    ///   cargo test --lib live_feed -- --ignored --nocapture
    #[tokio::test]
    #[ignore]
    async fn live_feed_monday_window() {
        let path = std::path::PathBuf::from(std::env::var("APPDATA").unwrap())
            .join("app.ori.orion")
            .join("state.json");
        let saved: serde_json::Value =
            serde_json::from_slice(&std::fs::read(path).expect("no state.json — import first")).unwrap();
        let url = saved["calendar_ics_url"].as_str().expect("no calendar_ics_url — import first");
        let (s, e) = monday_window();
        let meetings = fetch_meetings(url, s, e).await.unwrap();
        println!("live feed, Monday 2026-07-20: {} meeting(s)", meetings.len());
        for m in &meetings {
            println!("  s={} e={} title={:?} loc={:?}", m.start, m.end, m.title, m.location);
        }
        assert!(!meetings.is_empty(), "feed showed a Sprint Start Meeting on 2026-07-20");
    }
}
