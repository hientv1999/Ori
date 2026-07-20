# Ori — Go-to-Market Plan

> **Status:** Planning. Last updated 2026-07-11.
> **Scope:** Which platform to launch on, and the step-by-step sequence before shipping the initial 100-unit batch.
> Companion doc: [BOM_and_Pricing.md](BOM_and_Pricing.md).

---

## 1. Platform comparison (2026)

| | **Kickstarter** | **Indiegogo** | **Crowd Supply** | **Direct (Shopify pre-order)** |
|---|---|---|---|---|
| Platform fee | 5% | 5% | 5% | 0% (Shopify plan ~C$40/mo) |
| Payment processing | ~3–5% | ~3% + $0.20 | 2.9% + $0.30 | ~2.9% + $0.30 |
| **Total take** | **~8–10%** | **~8–10%** | **~8%** | **~3%** |
| Funding model | All-or-nothing | All-or-nothing (since Oct 2025) | All-or-nothing | N/A (you keep everything) |
| Audience | Largest; premium/design-led | Huge international reach (223 countries); price-sensitive gadget buyers | Small but **hardware/engineer-native**; dev boards, open hardware | Only who you drive there |
| Success rate | 42.7% (Jan 2026) | ~18–30% | "2× Kickstarter/Indiegogo" (curated) | N/A |
| Fulfillment help | None (DIY) | None (DIY) | **Hands-on: reviews mfg plan, handles fulfillment & logistics** | DIY |
| Post-campaign sales | Free pledge manager | InDemand (5–15%) | Ongoing store | Native |
| Canadian creator? | ✅ Yes (verified CA account/entity) | ✅ Yes | ✅ Yes | ✅ Yes |
| Discovery / built-in traffic | **High** | Medium | Low-but-targeted | **None** |

Payout timing (Kickstarter CA): funds reach your bank ~17–28 days after campaign end.

---

## 2. Recommendation

**Primary: Kickstarter.** For a first-time hardware launch aimed at a broad prosumer/office audience, it's the best fit:
- Ori is a design-led consumer desk object — Kickstarter's sweet spot (vs. Indiegogo's more price-sensitive, functional-gadget crowd).
- Highest built-in discovery and the strongest trust signal for a brand nobody's heard of yet.
- Free pledge manager for post-campaign upsells/address collection.
- All-or-nothing funding is *protective* for you: if you don't hit the number that makes a 100-unit run viable, you're not obligated to ship at a loss.

**Strong alternative: Crowd Supply — consider seriously if you lean into Ori's engineering heritage** (ESP32-S3, open-ish hardware, USB-CDC/BLE protocol). Their hands-on manufacturing + fulfillment support materially de-risks a first-time hardware founder, their audience is exactly the technical early adopter who tolerates a 3D-printed first batch, and their success rate is the highest of the three. The trade-off: smaller reach and a curation/onboarding process that's slower to start.

**Skip for now:** Indiegogo (lower success rate, audience skews price-sensitive, no offsetting advantage over Kickstarter for this product) and pure Direct/Shopify (0% platform cut is tempting, but with no built-in audience you'd carry 100% of the demand-generation risk — better as the *post-campaign* sales channel).

> **Decision rule:** Consumer-desk-accessory story + want max reach → **Kickstarter**. Maker/engineer story + want fulfillment hand-holding → **Crowd Supply**. Either way, run **Shopify as the always-on store afterward** for continued sales.

> **Reframe worth considering:** crowdfunding exists to *validate demand and fund a batch you can't otherwise afford*. If you can self-fund 100 units and mainly want to sell them, a **waitlist → Shopify pre-order** is simpler and keeps ~7% more margin. Use a campaign if you want the validation, the discovery, and the certification-amortizing volume (a campaign that sells 300+ units fixes the NRE math in [BOM_and_Pricing.md](BOM_and_Pricing.md) §3).

---

## 3. Step-by-step before hitting the market

Ordered by dependency — each phase gates the next. Rough sequence, ~3–5 months end to end.

### Phase 0 — Prove the product (do first, non-negotiable)
1. **Finalize firmware + Orion app to a demoable state.** Per `CLAUDE.md` milestones: M6 (Orion PC app) is in progress; M7 (end-to-end integration) and M8 (hardening) are open. **You cannot film a campaign video or ship a batch until the full loop works** (calendar → Orion → BLE → Ori → screen). This is the real critical path — not the finance.
2. **Build 3–5 hand-assembled units** that survive daily desk use for a couple of weeks. Shake out reliability (BLE reconnect, OTA, thermal, enclosure fit).

### Phase 1 — Lock the numbers
3. **Get real quotes** for every estimated BOM line (enclosure, packaging, hardware kit, labor) — replace the estimates in [BOM_and_Pricing.md](BOM_and_Pricing.md) §1.
4. **Get a firm certification quote** (FCC + ISED bundle) from an accredited lab and **decide the certification strategy** (R&D-amortized / larger batch / uncertified beta — §3 of the BOM doc). This decision drives your funding goal and price.
5. **Set the funding goal** = (batch cost + NRE + platform fees + shipping + buffer). At 100 units and C$150, model whether the campaign clears it; if not, either raise the target unit count or the price.

### Phase 2 — Manufacturing & fulfillment readiness
6. **Finalize the enclosure for repeatable printing** — print farm capacity, per-unit print time, post-processing (sand/prime/paint or accept raw), reject rate. Confirm 100 units is actually feasible on your printer(s) in a reasonable window.
7. **Write the assembly + firmware-flash + QC runbook** so units are built consistently. Define a pass/fail QC checklist.
8. **Plan logistics:** packaging supplier, shipping carrier + rates by region (charge backers shipping separately — don't absorb it), customs/HS code for cross-border, returns policy.
9. **Legal/tax:** register the business entity (needed for a Kickstarter *entity* account and clean accounting), confirm sales-tax handling (Kickstarter collects CA sales tax on your behalf), product-liability basics, and trademark check on "Ori" / "Orion" (a busy space — verify before you print it on boxes).

### Phase 3 — Campaign assets
10. **Product photography + a 60–90s video** — the single biggest driver of campaign success. Show the real device on a real desk doing the real thing (presence, meetings, notifications, media control). Your existing UI prototype (`Ori_UI_Prototype.html`) is a good on-screen asset, but backers need to see *hardware*.
11. **Write the campaign page:** problem → product → who it's for → reward tiers → risks & timeline (be honest about the 3D-printed first batch; frame as "founder's edition"). Reference the comp tier so C$150 reads as a deal.
12. **Reward tiers:** early-bird (limited qty, lowest price) → standard → maybe a 2-pack. Keep SKUs minimal for a 100-unit run.

### Phase 4 — Pre-launch audience (start during Phase 2–3, weeks ahead)
13. **Stand up a landing page + email waitlist** (`ori.app` already exists per the spec — add an email capture). **Campaigns that fund fast do it on day one from a pre-built list.** Aim for a few hundred emails before launch.
14. **Seed communities** where your buyer lives: r/ESP32, r/homelab, r/productivity, hardware/EE Discords, Hacker News (Show HN once live). Line up any press/newsletter contacts.
15. **Run a "notify on launch" Kickstarter pre-launch page** to collect follows (converts to day-one pledges).

### Phase 5 — Launch
16. **Launch to the waitlist first** (front-load pledges → algorithm favors you → organic discovery compounds).
17. **Fulfill through the pledge manager**, then flip continued sales to **Shopify** using campaign momentum + reviews.

---

## 4. Critical-path reality check

The finance is ready to finalize once quotes land, but **the true blocker is product completeness (Phase 0), not pricing.** Per `CLAUDE.md`, Orion (M6) is mid-build and integration/hardening (M7–M8) haven't started. Sequence the money work in parallel, but **do not launch a campaign until a real end-to-end unit works on a desk** — a crowdfunding campaign converts a working demo into orders; it can't substitute for one, and an over-promised hardware campaign that slips is the #1 way first-time hardware creators burn their backers and reputation.

---

## Sources
- [Kickstarter vs Indiegogo 2026 — Fundpop](https://fundpop.co/compare/kickstarter-vs-indiegogo)
- [Kickstarter vs Indiegogo 2026 — Blazon Agency](https://blazonagency.com/post/kickstarter-vs-indiegogo)
- [Crowd Supply vs Kickstarter — CrowdCrux](https://www.crowdcrux.com/crowd-supply-vs-kickstarter/)
- [Crowd Supply](https://www.crowdsupply.com/)
- [Kickstarter Fees: Canada](https://www.kickstarter.com/help/fees?country=CA)
- [Who is Eligible to Use Kickstarter?](https://updates.kickstarter.com/who-is-eligible-to-use-kickstarter/)
