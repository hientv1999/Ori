#pragma once

#include <stddef.h>
#include <stdint.h>

// Local-holiday lookup.
//
// Two data sources, both usable with NO live Orion connection once they've
// been set at least once (both persist to NVS — see nvs_store.h):
//
//   1. A compiled-in table of RULE-based holidays per country (Fixed-date,
//      Nth-weekday-of-month, and Easter-offset rules — e.g. "always July 4"
//      or "3rd Monday of January"). These are timeless: no fetching, no
//      per-year data, evaluated fresh from the rule on every lookup. Ori
//      only needs to know WHICH country's table to use (see set_country()).
//   2. A small NVS-cached list of Vietnam's Tet (lunar new year) dates,
//      computed by Orion (which has a proper lunisolar calendar library —
//      not practical to reimplement correctly in embedded C++) and pushed
//      once over BLE (Lunar Holiday List characteristic, ble-protocol.md).
//      Only ever consulted when country == VN.
//
// Once Orion has synced a country + (for VN) the lunar table ONE time, both
// keep working indefinitely with Ori fully offline — same persistence model
// as Profile/Photo/Time Off (ble-protocol.md §6.0/§7). Only a device that has
// NEVER synced with Orion at all (country defaults to None) shows nothing.
namespace holiday_data {

// Covers the primary country for each of Orion's 4 supported UI languages
// (English: US/CA/GB/AU, Vietnamese: VN, Spanish: ES/MX, French: CA/FR) plus
// Canada twice over (English+French). An unresolved/unsupported country
// (anything else) sends None — no rule table exists for it, so showing
// nothing is correct, not a placeholder guess (pc-app.md's holiday.rs).
enum class Country : uint8_t {
    None = 0,
    US   = 1,
    VN   = 2,
    CA   = 3,
    GB   = 4, // United Kingdom — ISO 3166-1 alpha-2 is "GB", not "UK"
    AU   = 5,
    ES   = 6,
    MX   = 7,
    FR   = 8,
};

struct Info {
    const char* name;
    const char* description;
};

// Region codes are per-country (region N means something different under US
// vs under CA) — 0 always means "no region selected / national-only" for
// every country. Meanings, and which regions actually carry any rules (many
// countries' subdivisions have no official difference from the national
// table and are intentionally left as no-ops):
//
//   CA: 1=BC 2=ON 3=AB 4=SK 5=MB 6=NB 7=NS 8=PE 9=NL 10=QC 11=YT 12=NT 13=NU
//   GB: 1=Scotland 2=NorthernIreland (England/Wales stay 0)
//   AU: 1=NSW 2=VIC 3=QLD 4=WA 5=SA 6=TAS 7=ACT 8=NT
//   ES: 1=Andalucía 2=Aragón 3=Asturias 4=Baleares 5=Canarias 6=Cantabria
//       7=CastillaYLeón 8=CastillaLaMancha 9=Cataluña 10=Valencia
//       11=Extremadura 12=Galicia 13=Madrid 14=Murcia 15=Navarra (no-op —
//       no officially fixed regional day found) 16=PaísVasco 17=LaRioja
//       18=Ceuta 19=Melilla
//   US: 1=MA 2=ME 3=TX 4=AK 5=HI (a modest, well-documented set — US state
//       holiday law is far less standardized than the others here, so this
//       intentionally isn't all 50 states)
//   FR: 1=AlsaceMoselle (data exists, but see pc-app.md — Orion's
//       geolocation resolves at the *region* level, e.g. "Grand Est", which
//       doesn't cleanly identify the 3 Alsace-Moselle *departments* inside
//       it, so auto-detection is a known gap; the rule table itself is
//       still here for when the region is known some other way)
//   VN, MX: no regional table — no well-documented official subdivision
//       variation found; region is always a no-op for these two.

// Load the NVS-persisted country + region + lunar-day cache into RAM. Call
// once at boot, after nvs::init().
void init();

// Combined lookup for (year, month [1-12], day [1-31]) under `country` +
// `region` (0 = national only). Region-specific rules are checked first (so
// a region can add a holiday the nation doesn't have, or override one it
// does — e.g. Quebec's "National Patriots' Day" instead of the national
// "Victoria Day" on the same date); national rules are checked next, unless
// the specific rule declares this `region` excluded (e.g. the national
// June King's Birthday date doesn't apply in Queensland or Western
// Australia, which use their own dates/rules instead). If country == VN and
// no rule matched, also checks the NVS-cached lunar (Tet) table. Returns
// nullptr if nothing matches. The debug-override hand-test data (see
// set_debug_override()) takes priority over all of this when enabled.
const Info* name_for(Country country, uint8_t region, int year, int month, int day);

// Active country — NVS-persisted (survives power cycles). Set by Orion via
// Device Settings (char 000E, key "g"). Default None.
void    set_country(Country country);
Country country();

// Active region within `country` (NVS-persisted, default 0/None) — see the
// per-country region code table above. Set by Orion via Device Settings
// (char 000E, key "j"). A region value with no meaning under the current
// country (e.g. leftover CA region code after switching to US) is simply
// never matched by name_for() — harmless, not validated against country.
void    set_region(uint8_t region);
uint8_t region();

// Replaces the NVS-cached lunar-new-year (Tet) date table. epoch_days are
// days since 1970-01-01 UTC (a uint16_t comfortably covers 1970-2100).
// Called once a Lunar Holiday List BLE write finishes reassembling. Persists
// immediately so it survives a power cycle without Orion needing to resend
// on every reconnect — this data changes essentially never once computed.
// count is capped internally at 200 entries (defensive; ~130 expected).
void set_lunar_days(const uint16_t* epoch_days, size_t count);

// Illustrative hand-test data (day 3 / day 20 of any month report as "Public
// Holiday", regardless of country) — for verifying the ring/text-color/
// subtitle rendering on real hardware without needing a real sync. Off by
// default. Only ever called from the ORI_DEBUG_SERIAL cycler.
void set_debug_override(bool enabled);

} // namespace holiday_data
