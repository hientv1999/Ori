#include "holiday_data.h"

#include <cstring>
#include <ctime>

#include "nvs_store.h"
#include "ori_log.h"

namespace holiday_data {

namespace {

enum class RuleType {
    Fixed,          // always (month, day)
    NthWeekday,     // the nth (1-4) occurrence of `weekday` in `month`
    EasterOffset,   // `offset_days` from Easter Sunday (may cross month/year)
    LastWeekday,    // the LAST occurrence of `weekday` in `month`
    WeekdayBefore,  // the most recent `weekday` strictly before (month, day)
    WeekdayOnOrAfter, // the first `weekday` on or after (month, day)
};

struct HolidayRule {
    RuleType    type;
    int         month;       // 1-12 — Fixed/NthWeekday/LastWeekday/WeekdayBefore/WeekdayOnOrAfter
    int         day;         // 1-31 — Fixed/WeekdayBefore/WeekdayOnOrAfter (reference date)
    int         weekday;     // 0=Monday..6=Sunday — every rule but Fixed/EasterOffset
    int         nth;         // 1-4 — NthWeekday
    int         offset_days; // EasterOffset
    const char* name;
    const char* description;
    // Trailing — every existing entry below that doesn't specify these two
    // gets region=0/exclude_mask=0 for free (plain aggregate-init rule: an
    // initializer list shorter than the struct zero-fills the rest), so none
    // of the pre-existing national entries needed editing to add regions.
    int         region;         // 0 = universal (every region of this country); N = only region N
    uint16_t    exclude_mask;   // only meaningful when region==0: bit N set = region N does NOT get this rule
};

// US public holidays (national) + a modest, well-documented set of state
// holidays — US state holiday law is far less standardized than Canada's or
// Australia's, so this intentionally isn't all 50 states (pc-app.md).
// Region codes: 1=MA 2=ME 3=TX 4=AK 5=HI.
constexpr HolidayRule US_RULES[] = {
    {RuleType::NthWeekday,  4,  0, 0, 3,  0, "Patriots' Day",
        "Commemorates the 1775 battles of Lexington and Concord; observed the third Monday of April.", 1, 0},
    {RuleType::NthWeekday,  4,  0, 0, 3,  0, "Patriots' Day",
        "Commemorates the 1775 battles of Lexington and Concord; observed the third Monday of April.", 2, 0},
    {RuleType::Fixed,       3,  2, 0, 0,  0, "Texas Independence Day",
        "Commemorates the 1836 signing of the Texas Declaration of Independence.", 3, 0},
    {RuleType::LastWeekday, 3,  0, 0, 0,  0, "Seward's Day",
        "Commemorates the 1867 signing of the Alaska Purchase treaty; observed the last Monday of March.", 4, 0},
    {RuleType::Fixed,      10, 18, 0, 0,  0, "Alaska Day",
        "Commemorates the 1867 formal transfer of Alaska from Russia to the United States.", 4, 0},
    {RuleType::Fixed,       6, 11, 0, 0,  0, "Kamehameha Day",
        "Honors King Kamehameha I, who united the Hawaiian Islands.", 5, 0},
    {RuleType::Fixed,       1,  1, 0, 0,  0, "New Year's Day",
        "Marks the first day of the Gregorian calendar year."},
    {RuleType::NthWeekday,  1,  0, 0, 3,  0, "Martin Luther King Jr. Day",
        "Honors the civil rights leader; observed the third Monday of January."},
    {RuleType::EasterOffset, 0, 0, 0, 0, -2, "Good Friday",
        "Commemorates the day of the crucifixion, observed the Friday before Easter Sunday."},
    {RuleType::Fixed,       7,  4, 0, 0,  0, "Independence Day",
        "Commemorates the 1776 adoption of the Declaration of Independence."},
    {RuleType::NthWeekday,  9,  0, 0, 1,  0, "Labor Day",
        "Honors the American labor movement; observed the first Monday of September."},
    {RuleType::NthWeekday, 11,  0, 3, 4,  0, "Thanksgiving Day",
        "A day of giving thanks, observed the fourth Thursday of November."},
    {RuleType::Fixed,      12, 25, 0, 0,  0, "Christmas Day",
        "Commemorates the birth of Jesus Christ, widely observed as a public holiday."},
};

// Vietnamese public holidays. Tet (lunar new year) is NOT here — it has no
// fixed Gregorian rule and comes from the NVS lunar cache instead (see
// is_lunar_match() below), only ever consulted for Country::VN. No
// well-documented official provincial variation found — region is a no-op.
constexpr HolidayRule VN_RULES[] = {
    {RuleType::Fixed, 1,  1, 0, 0, 0, "New Year's Day",
        "Marks the first day of the Gregorian calendar year."},
    {RuleType::Fixed, 4, 30, 0, 0, 0, "Liberation Day",
        "Commemorates the 1975 reunification of Vietnam, also known as Reunification Day."},
    {RuleType::Fixed, 5,  1, 0, 0, 0, "International Labor Day",
        "Honors workers and the labor movement; widely observed as a public holiday."},
    {RuleType::Fixed, 9,  2, 0, 0, 0, "National Day",
        "Commemorates Vietnam's 1945 Declaration of Independence."},
};

// Canadian federal statutory holidays + every province/territory's own
// well-documented statutory additions (Wikipedia's "Public holidays in
// Canada"). Region codes: 1=BC 2=ON 3=AB 4=SK 5=MB 6=NB 7=NS 8=PE 9=NL
// 10=QC 11=YT 12=NT 13=NU. Region-specific entries are listed FIRST so a
// same-date regional rename (Newfoundland's Memorial Day/Canada Day,
// Quebec's National Patriots' Day/Victoria Day) is found before the
// national entry for the same date — eval_rules() returns the first match.
constexpr HolidayRule CA_RULES[] = {
    // Newfoundland and Labrador — shares July 1 with the national Canada
    // Day below, but NL calls it "Memorial Day" first (WWI Beaumont-Hamel
    // commemoration), Canada Day second.
    {RuleType::Fixed,        7,  1, 0, 0,  0, "Memorial Day",
        "Commemorates the Newfoundland Regiment's losses at Beaumont-Hamel in 1916; also Canada Day.", 9, 0},
    // Quebec — "Journée nationale des Patriotes" replaces Victoria Day on
    // the same Monday-before-May-25 date.
    {RuleType::WeekdayBefore, 5, 25, 0, 0,  0, "National Patriots' Day",
        "Commemorates the Lower Canada Rebellion patriots; replaces Victoria Day in Quebec.", 10, 0},
    {RuleType::Fixed,         6, 24, 0, 0,  0, "Saint-Jean-Baptiste Day",
        "Quebec's National Holiday, honoring Quebec's patron saint and Francophone identity.", 10, 0},
    // Family Day and its provincial name variants — all 3rd Monday of Feb.
    {RuleType::NthWeekday, 2, 0, 0, 3, 0, "Family Day",
        "A day to spend with family; observed the third Monday of February.", 1, 0},
    {RuleType::NthWeekday, 2, 0, 0, 3, 0, "Family Day",
        "A day to spend with family; observed the third Monday of February.", 2, 0},
    {RuleType::NthWeekday, 2, 0, 0, 3, 0, "Family Day",
        "A day to spend with family; observed the third Monday of February.", 3, 0},
    {RuleType::NthWeekday, 2, 0, 0, 3, 0, "Family Day",
        "A day to spend with family; observed the third Monday of February.", 4, 0},
    {RuleType::NthWeekday, 2, 0, 0, 3, 0, "Family Day",
        "A day to spend with family; observed the third Monday of February.", 6, 0},
    {RuleType::NthWeekday, 2, 0, 0, 3, 0, "Islander Day",
        "Prince Edward Island's name for Family Day; observed the third Monday of February.", 8, 0},
    {RuleType::NthWeekday, 2, 0, 0, 3, 0, "Heritage Day",
        "Nova Scotia's name for Family Day; observed the third Monday of February.", 7, 0},
    {RuleType::NthWeekday, 2, 0, 0, 3, 0, "Louis Riel Day",
        "Manitoba's name for Family Day, honoring Métis leader Louis Riel; observed the third Monday of February.", 5, 0},
    // Civic Holiday and its provincial name variants — all 1st Monday of Aug.
    {RuleType::NthWeekday, 8, 0, 0, 1, 0, "British Columbia Day",
        "A civic holiday observed the first Monday of August.", 1, 0},
    {RuleType::NthWeekday, 8, 0, 0, 1, 0, "New Brunswick Day",
        "A civic holiday observed the first Monday of August.", 6, 0},
    {RuleType::NthWeekday, 8, 0, 0, 1, 0, "Saskatchewan Day",
        "A civic holiday observed the first Monday of August.", 4, 0},
    {RuleType::NthWeekday, 8, 0, 0, 1, 0, "Civic Holiday",
        "A territorial civic holiday observed the first Monday of August.", 12, 0},
    {RuleType::NthWeekday, 8, 0, 0, 1, 0, "Civic Holiday",
        "A territorial civic holiday observed the first Monday of August.", 13, 0},
    {RuleType::NthWeekday, 8, 0, 0, 2, 0, "Discovery Day",
        "Commemorates the 1896 discovery of gold that sparked the Klondike Gold Rush; observed the third Monday of August.", 11, 0},
    // National Day for Truth and Reconciliation — Sep 30, in the provinces/
    // territories that observe it as a statutory holiday.
    {RuleType::Fixed, 9, 30, 0, 0, 0, "National Day for Truth and Reconciliation",
        "Honors residential school survivors, their families, and communities.", 1, 0},
    {RuleType::Fixed, 9, 30, 0, 0, 0, "National Day for Truth and Reconciliation",
        "Honors residential school survivors, their families, and communities.", 8, 0},
    {RuleType::Fixed, 9, 30, 0, 0, 0, "National Day for Truth and Reconciliation",
        "Honors residential school survivors, their families, and communities.", 12, 0},
    {RuleType::Fixed, 9, 30, 0, 0, 0, "National Day for Truth and Reconciliation",
        "Honors residential school survivors, their families, and communities.", 13, 0},
    // National Indigenous Peoples Day — Jun 21, Yukon and NWT.
    {RuleType::Fixed, 6, 21, 0, 0, 0, "National Indigenous Peoples Day",
        "Celebrates the heritage and cultures of First Nations, Inuit, and Métis peoples.", 11, 0},
    {RuleType::Fixed, 6, 21, 0, 0, 0, "National Indigenous Peoples Day",
        "Celebrates the heritage and cultures of First Nations, Inuit, and Métis peoples.", 12, 0},
    // Northwest Territories also uniquely observes Easter Monday.
    {RuleType::EasterOffset, 0, 0, 0, 0, 1, "Easter Monday",
        "The day after Easter Sunday, observed as a statutory holiday in the Northwest Territories.", 12, 0},
    // Nunavut Day.
    {RuleType::Fixed, 7, 9, 0, 0, 0, "Nunavut Day",
        "Commemorates the 1993 signing of the Nunavut Land Claims Agreement.", 13, 0},
    // National baseline.
    {RuleType::Fixed,        1,  1, 0, 0,  0, "New Year's Day",
        "Marks the first day of the Gregorian calendar year."},
    {RuleType::EasterOffset, 0,  0, 0, 0, -2, "Good Friday",
        "Commemorates the day of the crucifixion, observed the Friday before Easter Sunday."},
    {RuleType::WeekdayBefore, 5, 25, 0, 0,  0, "Victoria Day",
        "Honors Queen Victoria's birthday; observed the Monday preceding May 25."},
    {RuleType::Fixed,        7,  1, 0, 0,  0, "Canada Day",
        "Commemorates the 1867 formation of the Canadian Confederation."},
    {RuleType::NthWeekday,   9,  0, 0, 1,  0, "Labour Day",
        "Honors the Canadian labour movement; observed the first Monday of September."},
    {RuleType::NthWeekday,  10,  0, 0, 2,  0, "Thanksgiving",
        "A day of giving thanks for the harvest; observed the second Monday of October."},
    {RuleType::Fixed,       11, 11, 0, 0,  0, "Remembrance Day",
        "Honors armed forces members who died in the line of duty."},
    {RuleType::Fixed,       12, 25, 0, 0,  0, "Christmas Day",
        "Commemorates the birth of Jesus Christ, widely observed as a public holiday."},
    {RuleType::Fixed,       12, 26, 0, 0,  0, "Boxing Day",
        "A traditional post-Christmas holiday, widely observed as a public holiday."},
};

// United Kingdom bank holidays — England & Wales' set (region 0) plus
// Scotland's and Northern Ireland's own additions. Region codes:
// 1=Scotland 2=NorthernIreland.
constexpr HolidayRule GB_RULES[] = {
    {RuleType::Fixed, 1,  2, 0, 0, 0, "New Year Holiday",
        "A second New Year bank holiday observed only in Scotland.", 1, 0},
    {RuleType::Fixed, 11, 30, 0, 0, 0, "St. Andrew's Day",
        "Honors Scotland's patron saint.", 1, 0},
    {RuleType::Fixed, 3, 17, 0, 0, 0, "Saint Patrick's Day",
        "Honors Ireland's patron saint; a bank holiday in Northern Ireland.", 2, 0},
    {RuleType::Fixed, 7, 12, 0, 0, 0, "Battle of the Boyne (Orangemen's Day)",
        "Commemorates the 1690 Battle of the Boyne; a bank holiday in Northern Ireland.", 2, 0},
    {RuleType::Fixed,        1,  1, 0, 0,  0, "New Year's Day",
        "Marks the first day of the Gregorian calendar year."},
    {RuleType::EasterOffset, 0,  0, 0, 0, -2, "Good Friday",
        "Commemorates the day of the crucifixion, observed the Friday before Easter Sunday."},
    {RuleType::EasterOffset, 0,  0, 0, 0,  1, "Easter Monday",
        "The day after Easter Sunday; a traditional bank holiday in England, Wales, and Northern Ireland."},
    {RuleType::NthWeekday,   5,  0, 0, 1,  0, "Early May Bank Holiday",
        "A spring public holiday; observed the first Monday of May."},
    {RuleType::LastWeekday,  5,  0, 0, 0,  0, "Spring Bank Holiday",
        "A late-spring public holiday; observed the last Monday of May."},
    {RuleType::LastWeekday,  8,  0, 0, 0,  0, "Summer Bank Holiday",
        "A late-summer public holiday; observed the last Monday of August."},
    {RuleType::Fixed,       12, 25, 0, 0,  0, "Christmas Day",
        "Commemorates the birth of Jesus Christ, widely observed as a public holiday."},
    {RuleType::Fixed,       12, 26, 0, 0,  0, "Boxing Day",
        "A traditional post-Christmas holiday, widely observed as a public holiday."},
};

// Australian public holidays — the common national set (region 0) plus
// every state/territory's own well-documented Labour Day date (there is no
// single national Labour Day — every state observes a different date under
// a different name) and a few other clean state-specific days. Region
// codes: 1=NSW 2=VIC 3=QLD 4=WA 5=SA 6=TAS 7=ACT 8=NT.
constexpr HolidayRule AU_RULES[] = {
    // Labour Day / May Day / Eight Hours Day — every state's own date.
    {RuleType::NthWeekday, 10, 0, 0, 1, 0, "Labour Day",
        "Honors the Australian labour movement; observed the first Monday of October.", 1, 0},
    {RuleType::NthWeekday,  3, 0, 0, 2, 0, "Labour Day",
        "Honors the Australian labour movement; observed the second Monday of March.", 2, 0},
    {RuleType::NthWeekday,  5, 0, 0, 1, 0, "Labour Day (May Day)",
        "Honors the Australian labour movement; observed the first Monday of May.", 3, 0},
    {RuleType::NthWeekday,  3, 0, 0, 1, 0, "Labour Day",
        "Honors the Australian labour movement; observed the first Monday of March.", 4, 0},
    {RuleType::NthWeekday, 10, 0, 0, 1, 0, "Labour Day",
        "Honors the Australian labour movement; observed the first Monday of October.", 5, 0},
    {RuleType::NthWeekday,  3, 0, 0, 2, 0, "Eight Hours Day",
        "Tasmania's name for Labour Day; observed the second Monday of March.", 6, 0},
    {RuleType::NthWeekday, 10, 0, 0, 1, 0, "Labour Day",
        "Honors the Australian labour movement; observed the first Monday of October.", 7, 0},
    {RuleType::NthWeekday,  5, 0, 0, 1, 0, "May Day",
        "Honors the Australian labour movement; observed the first Monday of May.", 8, 0},
    // Other clean state-specific days.
    {RuleType::NthWeekday,  6, 0, 0, 1, 0, "Western Australia Day",
        "Commemorates the 1829 founding of the Swan River Colony; observed the first Monday of June.", 4, 0},
    {RuleType::NthWeekday,  3, 0, 0, 2, 0, "Adelaide Cup Day",
        "A South Australian public holiday tied to the Adelaide Cup horse race; observed the second Monday of March.", 5, 0},
    {RuleType::NthWeekday,  3, 0, 0, 2, 0, "Canberra Day",
        "Commemorates the naming of Canberra; observed the second Monday of March.", 7, 0},
    {RuleType::NthWeekday,  8, 0, 0, 1, 0, "Picnic Day",
        "A Northern Territory public holiday; observed the first Monday of August.", 8, 0},
    // Queensland observes King's Birthday in October instead of the
    // national June date (excluded below via exclude_mask).
    {RuleType::NthWeekday, 10, 0, 0, 1, 0, "King's Birthday",
        "Celebrates the monarch's official birthday; observed the first Monday of October in Queensland.", 3, 0},
    // ACT's Reconciliation Day — first Monday on or after May 27.
    {RuleType::WeekdayOnOrAfter, 5, 27, 0, 0, 0, "Reconciliation Day",
        "Marks the start of National Reconciliation Week; observed the first Monday on or after May 27.", 7, 0},
    // South Australia's Proclamation Day replaces Boxing Day on Dec 26.
    {RuleType::Fixed, 12, 26, 0, 0, 0, "Proclamation Day",
        "Commemorates the 1836 proclamation of South Australia as a British province; replaces Boxing Day.", 5, 0},
    // National baseline. King's Birthday excludes QLD (has its own October
    // date above) and WA (no fixed rule found — the Governor proclaims a
    // variable date most years, so WA intentionally gets no entry at all
    // rather than a guess).
    {RuleType::Fixed,        1,  1, 0, 0,  0, "New Year's Day",
        "Marks the first day of the Gregorian calendar year."},
    {RuleType::Fixed,        1, 26, 0, 0,  0, "Australia Day",
        "Commemorates the 1788 arrival of the First Fleet at Sydney Cove."},
    {RuleType::EasterOffset, 0,  0, 0, 0, -2, "Good Friday",
        "Commemorates the day of the crucifixion, observed the Friday before Easter Sunday."},
    {RuleType::EasterOffset, 0,  0, 0, 0,  1, "Easter Monday",
        "The day after Easter Sunday, widely observed as a public holiday."},
    {RuleType::Fixed,        4, 25, 0, 0,  0, "Anzac Day",
        "Honors the Australian and New Zealand Army Corps and all who served."},
    {RuleType::NthWeekday,   6,  0, 0, 2,  0, "King's Birthday",
        "Celebrates the monarch's official birthday; observed the second Monday of June in most states.",
        0, (1u << 3) | (1u << 4)},
    {RuleType::Fixed,       12, 25, 0, 0,  0, "Christmas Day",
        "Commemorates the birth of Jesus Christ, widely observed as a public holiday."},
    {RuleType::Fixed,       12, 26, 0, 0,  0, "Boxing Day",
        "A traditional post-Christmas holiday, widely observed as a public holiday."},
};

// Spanish national public holidays (region 0) plus every autonomous
// community's own regional day ("fiesta autonómica" — Wikipedia's "Public
// holidays in Spain"). Region codes: 1=Andalucía 2=Aragón 3=Asturias
// 4=Baleares 5=Canarias 6=Cantabria 7=CastillaYLeón 8=CastillaLaMancha
// 9=Cataluña 10=Valencia 11=Extremadura 12=Galicia 13=Madrid 14=Murcia
// 15=Navarra (no-op — no officially fixed regional day found) 16=PaísVasco
// 17=LaRioja 18=Ceuta 19=Melilla.
constexpr HolidayRule ES_RULES[] = {
    {RuleType::Fixed, 2, 28, 0, 0, 0, "Día de Andalucía",
        "Andalusia's regional day, commemorating the 1980 autonomy referendum.", 1, 0},
    {RuleType::Fixed, 4, 23, 0, 0, 0, "Día de Aragón",
        "Aragón's regional day, coinciding with the feast of Saint George.", 2, 0},
    {RuleType::Fixed, 9,  8, 0, 0, 0, "Día de Asturias",
        "Asturias's regional day.", 3, 0},
    {RuleType::Fixed, 3,  1, 0, 0, 0, "Dia de les Illes Balears",
        "The Balearic Islands' regional day.", 4, 0},
    {RuleType::Fixed, 5, 30, 0, 0, 0, "Día de Canarias",
        "The Canary Islands' regional day, commemorating the 1983 first regional parliament session.", 5, 0},
    {RuleType::Fixed, 7, 28, 0, 0, 0, "Día de las Instituciones de Cantabria",
        "Cantabria's regional day.", 6, 0},
    {RuleType::Fixed, 4, 23, 0, 0, 0, "Día de Castilla y León",
        "Castile and León's regional day, coinciding with the feast of Saint George.", 7, 0},
    {RuleType::Fixed, 5, 31, 0, 0, 0, "Día de Castilla-La Mancha",
        "Castile-La Mancha's regional day.", 8, 0},
    {RuleType::Fixed, 9, 11, 0, 0, 0, "Diada Nacional de Catalunya",
        "Catalonia's National Day, commemorating the fall of Barcelona in 1714.", 9, 0},
    {RuleType::Fixed, 10, 9, 0, 0, 0, "Dia de la Comunitat Valenciana",
        "The Valencian Community's regional day.", 10, 0},
    {RuleType::Fixed, 9,  8, 0, 0, 0, "Día de Extremadura",
        "Extremadura's regional day.", 11, 0},
    {RuleType::Fixed, 7, 25, 0, 0, 0, "Santiago Apóstol (Día da Patria Galega)",
        "Galicia's regional day, honoring Saint James the Apostle, patron saint of Galicia.", 12, 0},
    {RuleType::Fixed, 5,  2, 0, 0, 0, "Fiesta de la Comunidad de Madrid",
        "The Community of Madrid's regional day.", 13, 0},
    {RuleType::Fixed, 6,  9, 0, 0, 0, "Día de la Región de Murcia",
        "The Region of Murcia's regional day.", 14, 0},
    {RuleType::Fixed, 10, 25, 0, 0, 0, "Euskadi Eguna",
        "The Basque Country's regional day.", 16, 0},
    {RuleType::Fixed, 6,  9, 0, 0, 0, "Día de La Rioja",
        "La Rioja's regional day.", 17, 0},
    {RuleType::Fixed, 9,  2, 0, 0, 0, "Día de Ceuta",
        "Ceuta's local day.", 18, 0},
    {RuleType::Fixed, 9, 17, 0, 0, 0, "Día de Melilla",
        "Melilla's local day.", 19, 0},
    // National baseline.
    {RuleType::Fixed,        1,  1, 0, 0,  0, "New Year's Day",
        "Marks the first day of the Gregorian calendar year."},
    {RuleType::Fixed,        1,  6, 0, 0,  0, "Epiphany",
        "Also known as Three Kings' Day, celebrating the visit of the Magi."},
    {RuleType::EasterOffset, 0,  0, 0, 0, -2, "Good Friday",
        "Commemorates the day of the crucifixion, observed the Friday before Easter Sunday."},
    {RuleType::Fixed,        5,  1, 0, 0,  0, "Labour Day",
        "Honors workers and the labour movement; widely observed as a public holiday."},
    {RuleType::Fixed,        8, 15, 0, 0,  0, "Assumption of Mary",
        "Commemorates the bodily assumption of the Virgin Mary into heaven."},
    {RuleType::Fixed,       10, 12, 0, 0,  0, "National Day of Spain",
        "Commemorates Christopher Columbus's 1492 arrival in the Americas."},
    {RuleType::Fixed,       11,  1, 0, 0,  0, "All Saints' Day",
        "Honors all saints, known and unknown."},
    {RuleType::Fixed,       12,  6, 0, 0,  0, "Constitution Day",
        "Commemorates the 1978 referendum ratifying Spain's constitution."},
    {RuleType::Fixed,       12,  8, 0, 0,  0, "Immaculate Conception",
        "Commemorates the Virgin Mary's conception free of original sin."},
    {RuleType::Fixed,       12, 25, 0, 0,  0, "Christmas Day",
        "Commemorates the birth of Jesus Christ, widely observed as a public holiday."},
};

// Mexican official rest days (Ley Federal del Trabajo Art. 74) — the one-off
// "Transmisión del Poder Ejecutivo Federal" (once every 6 years) is
// excluded. No well-documented, widely-standardized state-level statutory
// holidays found across Mexico's 32 states — region is a no-op here.
constexpr HolidayRule MX_RULES[] = {
    {RuleType::Fixed,       1,  1, 0, 0,  0, "New Year's Day",
        "Marks the first day of the Gregorian calendar year."},
    {RuleType::NthWeekday,  2,  0, 0, 1,  0, "Constitution Day",
        "Commemorates the 1917 constitution; observed the first Monday of February."},
    {RuleType::NthWeekday,  3,  0, 0, 3,  0, "Benito Juárez's Birthday",
        "Honors former president Benito Juárez; observed the third Monday of March."},
    {RuleType::Fixed,       5,  1, 0, 0,  0, "Labour Day",
        "Honors workers and the labour movement; widely observed as a public holiday."},
    {RuleType::Fixed,       9, 16, 0, 0,  0, "Independence Day",
        "Commemorates the start of the 1810 War of Independence."},
    {RuleType::NthWeekday, 11,  0, 0, 3,  0, "Revolution Day",
        "Commemorates the start of the 1910 Mexican Revolution; observed the third Monday of November."},
    {RuleType::Fixed,      12, 25, 0, 0,  0, "Christmas Day",
        "Commemorates the birth of Jesus Christ, widely observed as a public holiday."},
};

// French public holidays (jours fériés légaux) — national (region 0) plus
// the well-documented Alsace-Moselle carve-out (a 19th-century concordat
// law, still in force, that grants the Bas-Rhin, Haut-Rhin, and Moselle
// départements two extra days). Region code: 1=AlsaceMoselle. NOTE: Orion's
// geolocation resolves at the *region* level (e.g. "Grand Est", which
// merged historic Alsace with Champagne-Ardenne/Lorraine in 2016) — that's
// coarser than the 3 départements this actually applies to, so automatic
// detection is a known gap (pc-app.md); this table exists for when the
// region is known some other way.
constexpr HolidayRule FR_RULES[] = {
    {RuleType::EasterOffset, 0, 0, 0, 0, -2, "Good Friday",
        "Commemorates the day of the crucifixion; a legal holiday only in Alsace-Moselle.", 1, 0},
    {RuleType::Fixed, 12, 26, 0, 0, 0, "St. Stephen's Day",
        "Honors the first Christian martyr; a legal holiday only in Alsace-Moselle.", 1, 0},
    // National baseline.
    {RuleType::Fixed,        1,  1, 0, 0,   0, "New Year's Day",
        "Marks the first day of the Gregorian calendar year."},
    {RuleType::EasterOffset, 0,  0, 0, 0,   1, "Easter Monday",
        "The day after Easter Sunday, widely observed as a public holiday."},
    {RuleType::Fixed,        5,  1, 0, 0,   0, "Labour Day",
        "Honors workers and the labour movement; widely observed as a public holiday."},
    {RuleType::Fixed,        5,  8, 0, 0,   0, "Victory in Europe Day",
        "Commemorates the Allied victory over Nazi Germany in 1945."},
    {RuleType::EasterOffset, 0,  0, 0, 0,  39, "Ascension Day",
        "Commemorates the ascension of Jesus Christ into heaven."},
    {RuleType::EasterOffset, 0,  0, 0, 0,  50, "Whit Monday",
        "The day after Pentecost, widely observed as a public holiday."},
    {RuleType::Fixed,        7, 14, 0, 0,   0, "Bastille Day",
        "Commemorates the 1789 storming of the Bastille."},
    {RuleType::Fixed,        8, 15, 0, 0,   0, "Assumption of Mary",
        "Commemorates the bodily assumption of the Virgin Mary into heaven."},
    {RuleType::Fixed,       11,  1, 0, 0,   0, "All Saints' Day",
        "Honors all saints, known and unknown."},
    {RuleType::Fixed,       11, 11, 0, 0,   0, "Armistice Day",
        "Commemorates the armistice that ended fighting in the First World War."},
    {RuleType::Fixed,       12, 25, 0, 0,   0, "Christmas Day",
        "Commemorates the birth of Jesus Christ, widely observed as a public holiday."},
};

// Weekday of (year, month, day), shifted so Monday=0..Sunday=6 (matches this
// codebase's existing convention — see screen_calendar.cpp's
// (tm_wday + 6) % 7). Noon-anchored to sidestep DST-boundary edge cases,
// same idiom used throughout screen_calendar.cpp/screen_no_meetings.cpp.
int weekday_of(int year, int month, int day) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    time_t tt = mktime(&t);
    struct tm norm;
    localtime_r(&tt, &norm);
    return (norm.tm_wday + 6) % 7;
}

// Days in (year, month) via mktime()'s own tm_mday=0 rollback-to-previous-
// month trick — same idiom screen_calendar.cpp's own days-in-month
// computation already relies on.
int days_in_month(int year, int month) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month; // 0-based "next" month; tm_mday=0 rolls back to last day of `month`
    t.tm_mday = 0;
    t.tm_hour = 12;
    time_t tt = mktime(&t);
    struct tm norm;
    localtime_r(&tt, &norm);
    return norm.tm_mday;
}

// Day-of-month of the `nth` (1-4) occurrence of `weekday` (0=Mon..6=Sun) in
// (year, month).
int nth_weekday_day(int year, int month, int weekday, int nth) {
    int first_dow = weekday_of(year, month, 1);
    int offset = (weekday - first_dow + 7) % 7;
    return 1 + offset + (nth - 1) * 7;
}

// Day-of-month of the LAST occurrence of `weekday` in (year, month) — e.g.
// UK's Spring/Summer Bank Holiday ("last Monday of May/August").
int last_weekday_day(int year, int month, int weekday) {
    int dim = days_in_month(year, month);
    int last_dow = weekday_of(year, month, dim);
    int delta = (last_dow - weekday + 7) % 7;
    return dim - delta;
}

// Easter Sunday (Gregorian) via the Anonymous Gregorian algorithm (Meeus/
// Jones/Butcher) — pure integer arithmetic, no astronomy (unlike a real
// lunisolar calendar, Easter's "moon" is a fictional ecclesiastical
// approximation designed centuries ago to be hand-computable). Verified by
// hand against known dates before use: 2024 -> Mar 31, 2025 -> Apr 20.
void easter_month_day(int year, int* out_month, int* out_day) {
    int a  = year % 19;
    int b  = year / 100;
    int c  = year % 100;
    int d  = b / 4;
    int e  = b % 4;
    int f  = (b + 8) / 25;
    int g  = (b - f + 1) / 3;
    int h  = (19 * a + b - d - g + 15) % 30;
    int i2 = c / 4;
    int k  = c % 4;
    int l  = (32 + 2 * e + 2 * i2 - h - k) % 7;
    int m  = (a + 11 * h + 22 * l) / 451;
    *out_month = (h + l - 7 * m + 114) / 31;
    *out_day   = ((h + l - 7 * m + 114) % 31) + 1;
}

// Adds `delta` days to (year, month, day) in place, via mktime()'s own
// out-of-range-tm_mday normalization — same idiom screen_calendar.cpp's
// nav_month()/render_into() already rely on for month/year rollover.
void add_days(int* year, int* month, int* day, int delta) {
    struct tm t = {};
    t.tm_year = *year - 1900;
    t.tm_mon  = *month - 1;
    t.tm_mday = *day + delta;
    t.tm_hour = 12;
    time_t tt = mktime(&t);
    struct tm norm;
    localtime_r(&tt, &norm);
    *year  = norm.tm_year + 1900;
    *month = norm.tm_mon + 1;
    *day   = norm.tm_mday;
}

// Moves (year, month, day) in place to the most recent occurrence of
// `weekday` STRICTLY BEFORE it — e.g. Canada's Victoria Day, legally "the
// Monday immediately preceding May 25" (if May 25 IS a Monday, Victoria Day
// is May 18, not May 25 itself — "preceding" means strictly earlier).
void weekday_before_date(int* year, int* month, int* day, int weekday) {
    int dow = weekday_of(*year, *month, *day);
    int delta = (dow - weekday + 7) % 7;
    if (delta == 0) delta = 7;
    add_days(year, month, day, -delta);
}

// Moves (year, month, day) in place to the first occurrence of `weekday` ON
// OR AFTER it — e.g. ACT's Reconciliation Day, "the first Monday on or
// after May 27" (unlike weekday_before_date, equality counts: if the
// reference date itself IS that weekday, it doesn't move at all).
void weekday_on_or_after_date(int* year, int* month, int* day, int weekday) {
    int dow = weekday_of(*year, *month, *day);
    int delta = (weekday - dow + 7) % 7;
    add_days(year, month, day, delta);
}

// Days since 1970-01-01 (noon-anchored local time — safe for this table's
// currently-supported countries, whose UTC offsets never push a local-noon
// instant across a UTC day boundary).
long epoch_day_for(int year, int month, int day) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    time_t tt = mktime(&t);
    return (long)(tt / 86400);
}

const Info* eval_rules(const HolidayRule* rules, size_t count, int year, int month, int day, uint8_t region) {
    static Info info;
    for (size_t idx = 0; idx < count; ++idx) {
        const HolidayRule& r = rules[idx];
        if (r.region != 0) {
            // Region-specific rule — only applies to that exact region.
            if (r.region != (int)region) continue;
        } else if (region != 0 && r.exclude_mask != 0 && (r.exclude_mask & (1u << region))) {
            // Universal rule, but explicitly excluded for this region (e.g.
            // AU's national King's Birthday doesn't apply in Queensland/WA).
            continue;
        }
        bool match = false;
        switch (r.type) {
            case RuleType::Fixed:
                match = (month == r.month && day == r.day);
                break;
            case RuleType::NthWeekday:
                if (month == r.month) {
                    match = (day == nth_weekday_day(year, r.month, r.weekday, r.nth));
                }
                break;
            case RuleType::LastWeekday:
                if (month == r.month) {
                    match = (day == last_weekday_day(year, r.month, r.weekday));
                }
                break;
            case RuleType::WeekdayBefore: {
                int wy = year, wm = r.month, wd = r.day;
                weekday_before_date(&wy, &wm, &wd, r.weekday);
                match = (wy == year && wm == month && wd == day);
                break;
            }
            case RuleType::WeekdayOnOrAfter: {
                int wy = year, wm = r.month, wd = r.day;
                weekday_on_or_after_date(&wy, &wm, &wd, r.weekday);
                match = (wy == year && wm == month && wd == day);
                break;
            }
            case RuleType::EasterOffset: {
                int ey = year, em, ed;
                easter_month_day(year, &em, &ed);
                add_days(&ey, &em, &ed, r.offset_days);
                match = (ey == year && em == month && ed == day);
                break;
            }
        }
        if (match) {
            info.name = r.name;
            info.description = r.description;
            return &info;
        }
    }
    return nullptr;
}

// ── Module state ────────────────────────────────────────────────────────

Country  g_country = Country::None;
uint8_t  g_region = 0;
bool     g_debug_override = false;

constexpr size_t MAX_LUNAR_DAYS = 200; // ~130 expected for a 1970-2100 range
uint16_t g_lunar_days[MAX_LUNAR_DAYS];
size_t   g_lunar_count = 0;

bool is_lunar_match(int year, int month, int day) {
    long ed = epoch_day_for(year, month, day);
    for (size_t i = 0; i < g_lunar_count; ++i) {
        if ((long)g_lunar_days[i] == ed) return true;
    }
    return false;
}

const char* country_name(Country country) {
    switch (country) {
        case Country::US: return "US";
        case Country::VN: return "VN";
        case Country::CA: return "CA";
        case Country::GB: return "GB";
        case Country::AU: return "AU";
        case Country::ES: return "ES";
        case Country::MX: return "MX";
        case Country::FR: return "FR";
        case Country::None: default: return "None";
    }
}

// Dumps every matching holiday day in `year` for the current country/region/
// lunar cache to the serial log — a debugging aid (fires on every Device
// Settings "g"/"j" write, i.e. every Orion (re)connect, ble-protocol.md
// §6.4) so a wrong/missing highlight can be diagnosed from the boot log
// alone.
void log_holidays_for_year(Country country, uint8_t region, int year) {
    LOG("[holiday] country=%s (%d) region=%u lunar_days_cached=%u\n",
        country_name(country), (int)country, (unsigned)region, (unsigned)g_lunar_count);
    if (country == Country::None) {
        LOG("[holiday] country is None — nothing will ever be highlighted until Orion resolves a supported location\n");
        return;
    }
    int found = 0;
    for (int month = 1; month <= 12; ++month) {
        int dim = days_in_month(year, month);
        for (int day = 1; day <= dim; ++day) {
            const Info* info = name_for(country, region, year, month, day);
            if (info) {
                LOG("[holiday]   %04d-%02d-%02d  %s\n", year, month, day, info->name);
                ++found;
            }
        }
    }
    LOG("[holiday] %d holiday day(s) found for %d\n", found, year);
}

} // namespace

void init() {
    g_country = static_cast<Country>(nvs::get_holiday_country());
    g_region = nvs::get_holiday_region();
    g_lunar_count = nvs::get_lunar_days(g_lunar_days, MAX_LUNAR_DAYS);
}

const Info* name_for(Country country, uint8_t region, int year, int month, int day) {
    // Hand-test demo data takes priority for its two fixed days, but doesn't
    // suppress real data on any other day — lets a developer verify the
    // rendering treatment and real country holidays at the same time.
    if (g_debug_override && (day == 3 || day == 20)) {
        static Info debug_info{
            "Public Holiday",
            "Illustrative demo data (ORI_DEBUG_SERIAL 'h' key) - no real data source configured yet."
        };
        return &debug_info;
    }

    const Info* rule_match = nullptr;
    switch (country) {
        case Country::US: rule_match = eval_rules(US_RULES, sizeof(US_RULES) / sizeof(US_RULES[0]), year, month, day, region); break;
        case Country::VN: rule_match = eval_rules(VN_RULES, sizeof(VN_RULES) / sizeof(VN_RULES[0]), year, month, day, region); break;
        case Country::CA: rule_match = eval_rules(CA_RULES, sizeof(CA_RULES) / sizeof(CA_RULES[0]), year, month, day, region); break;
        case Country::GB: rule_match = eval_rules(GB_RULES, sizeof(GB_RULES) / sizeof(GB_RULES[0]), year, month, day, region); break;
        case Country::AU: rule_match = eval_rules(AU_RULES, sizeof(AU_RULES) / sizeof(AU_RULES[0]), year, month, day, region); break;
        case Country::ES: rule_match = eval_rules(ES_RULES, sizeof(ES_RULES) / sizeof(ES_RULES[0]), year, month, day, region); break;
        case Country::MX: rule_match = eval_rules(MX_RULES, sizeof(MX_RULES) / sizeof(MX_RULES[0]), year, month, day, region); break;
        case Country::FR: rule_match = eval_rules(FR_RULES, sizeof(FR_RULES) / sizeof(FR_RULES[0]), year, month, day, region); break;
        case Country::None: break;
    }
    if (rule_match) return rule_match;

    if (country == Country::VN && is_lunar_match(year, month, day)) {
        static Info lunar_info{
            "Tet Nguyen Dan",
            "Vietnam's most important holiday, marking the start of the lunar new year; its Gregorian date shifts every year."
        };
        return &lunar_info;
    }
    return nullptr;
}

void set_country(Country country) {
    g_country = country;
    nvs::set_holiday_country(static_cast<uint8_t>(country));

    // Debug dump — see log_holidays_for_year()'s own comment.
    time_t now = time(nullptr);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    log_holidays_for_year(country, g_region, now_tm.tm_year + 1900);
}

Country country() {
    return g_country;
}

void set_region(uint8_t region) {
    g_region = region;
    nvs::set_holiday_region(region);

    // Same debug dump as set_country() — a region change is just as worth
    // seeing refreshed in the log. Harmless if this fires right alongside a
    // set_country() call on the same reconnect (both push every time,
    // ble-protocol.md §6.4) — just two prints of the same final state.
    time_t now = time(nullptr);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    log_holidays_for_year(g_country, region, now_tm.tm_year + 1900);
}

uint8_t region() {
    return g_region;
}

void set_lunar_days(const uint16_t* epoch_days, size_t count) {
    if (count > MAX_LUNAR_DAYS) count = MAX_LUNAR_DAYS;
    if (epoch_days && count > 0) {
        memcpy(g_lunar_days, epoch_days, count * sizeof(uint16_t));
    }
    g_lunar_count = count;
    nvs::set_lunar_days(g_lunar_days, g_lunar_count);
}

void set_debug_override(bool enabled) {
    g_debug_override = enabled;
}

} // namespace holiday_data
