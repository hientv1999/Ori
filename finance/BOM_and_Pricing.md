# Ori — BOM & Pricing

> **Status:** Working estimate, not a quote. Last updated 2026-07-11.
> **FX assumption:** 1 USD = 1.41 CAD (Bank of Canada / US Fed H.10, 2026-07-11). Parts sourced from USD-billing suppliers (Waveshare / China / US), so FX moves shift landed cost even when nothing else changes.
> **Batch size:** 100 units (initial pilot batch).

---

## 1. Bill of Materials — per unit @ qty 100 (CAD)

| Component | Cost/unit (CAD) | (USD ~) | Status |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-4.3 module (16 MB flash / 8 MB PSRAM; integrates ESP32-S3, GT911 touch, ST7701 800×480 RGB LCD, CH422G expander, USB-C) | **C$38.59** | $27.37 | ✅ **Confirmed quote** |
| Enclosure — in-house FDM 3D print (filament + print time + post-processing labor) | C$9–15 | $6–11 | Estimate |
| Feet, screws, stand hardware | C$1–3 | $1–2 | Estimate |
| Retail packaging (box, insert, quick-start card) | C$4–7 | $3–5 | Estimate |
| Assembly + firmware flash + QC labor (~20–25 min/unit) | C$8–14 | $6–10 | Estimate |
| **Per-unit landed cost** | **C$61–78** | **~$43–55** | **midpoint ≈ C$69** |

### Explicitly excluded (decided in planning)
- **LiPo backup battery** — skipped. `hardware.md` specs firmware/UI to behave as if no battery exists, so removing it changes no product behavior.
- **USB-C cable** — not shipped (BYO-cable; standard for USB-C peripherals).
- **USB-C wall adapter** — not shipped (BYO-charger).

---

## 2. One-time costs (NRE — NOT per-unit)

| Item | Cost (CAD) | Notes |
|---|---|---|
| FCC / ISED / CE certification | C$2,800–5,600 | Reduced scope because the ESP32-S3 module carries Espressif's own modular radio grant. The finished-product filing still needs Part 15B unintentional-emissions testing (the LCD/DMA/USB/enclosure as an assembled device) + verification the module was integrated per its grant conditions. This is normal practice — every wireless competitor (TRMNL, Divoom, Skylight, Kuando) filed for the assembled device, not just the radio chip. See §4. |
| Enclosure tooling | ~C$0 | In-house FDM 3D printing — no CNC programming or injection-mold tooling for this batch. Design time already sunk. |
| **If amortized over 100 units** | **+C$28–56/unit** | This is the single biggest per-unit swing at pilot scale — see §3 waterfall. |

---

## 3. Pricing

### Market comps (converted to CAD @ 1.41)

| Product | Category | Radio | Price (CAD) | Certified? |
|---|---|---|---|---|
| Luxafor Flag | Presence light, no display | None (wired USB) | C$61 | N/A (no radiator) |
| Kuando Busylight | Presence light, no display | Yes | C$66–78 | ✅ FCC `2AYIK1582` |
| **Divoom Times Gate** | Desk info display (clock/weather/notifs) | WiFi | **C$183–212** | ✅ FCC `A8I-TIMES-GATE` |
| **TRMNL (OG)** | 7.5" e-ink desk dashboard, indie | WiFi | **C$196–217** | ✅ FCC `2BFWO-OG` |
| Divoom Ditoo | Pixel display + speaker | BT | C$226 | ✅ (Divoom A8I grantee) |
| Skylight Calendar (15") | Large touchscreen family hub (+ subscription) | WiFi | C$268–282 | ✅ FCC `2AABK-150` |
| TRMNL X | 10.3" e-ink, larger | WiFi | C$309 | — |

**Ori's honest peer group is the Divoom Times Gate / TRMNL tier (~C$183–217):** small desk companion display, calendar/status/notifications, no forced subscription, indie/first-batch. Skylight and TRMNL X sit higher on the strength of larger screens, subscription attach, and established brand — positioning Ori there would overreach on batch one.

### Chosen price: **C$150**

Deliberately set *below* the direct-comp floor to de-risk purchase of an unproven first-batch product (3D-printed enclosure, no track record, no reviews).

### Per-unit profit waterfall at C$150

| Line | Amount (CAD) | Running |
|---|---|---|
| Sell price | +150.00 | 150.00 |
| BOM (midpoint) | −69.00 | 81.00 → **~54% gross margin** |
| Crowdfunding platform + payment fees (~9%) | −13.50 | 67.50 |
| NRE amortization (certification, midpoint C$42/unit @ 100 units) | −42.00 | **25.50** |
| **Net per unit** (backer pays shipping) | | **≈ C$25** |
| **Net over 100-unit batch** | | **≈ C$2,500** |

> ⚠️ **Key insight — certification is the margin killer at 100 units, not the BOM.** Spreading C$2,800–5,600 of certification across only 100 units costs C$28–56/unit. At C$150 the batch still clears a small profit *if* certification is treated as recoverable-over-future-batches R&D. If you must fully absorb certification in this batch alone, net drops toward break-even. Three ways to handle it:
> 1. **Treat certification as R&D investment** (recovered over batches 2, 3, …) — price stays C$150, batch one is ~break-even-to-small-profit and buys market validation. *(Recommended if the goal is learning + validation.)*
> 2. **Run a larger campaign** (e.g. 300–500 units) so certification amortizes to C$6–19/unit — the math improves dramatically above ~150 units.
> 3. **Beta/founder batch, uncertified, sold informally** to friends/early adopters only — lower enforcement risk, no certification spend, but not a public commercial sale (see §4).

### Alternative price points (vs ~C$69 BOM, before fees/NRE)

| Price | Gross margin | Note |
|---|---|---|
| **C$150** | ~54% | Chosen. Undercuts entire comp tier; thin after NRE at 100 units. |
| C$179 | ~61% | Smallest bump giving real headroom for NRE/shipping/returns; still under every comp. |
| C$199 | ~65% | Standard DTC-hardware margin; aligns with Times Gate/TRMNL comp tier. |

---

## 4. Certification note (why the module's cert isn't enough)

The ESP32-S3 module's FCC/IC **modular grant** covers only the radio transmitter, tested in isolation. It does **not** cover:
1. **Unintentional-radiator emissions (Part 15 Subpart B / ICES-003)** — the assembled Ori (12 MHz RGB LCD with continuous DMA, CH422G expander, USB-C, cabling, enclosure) can radiate noise independent of the certified radio. Must be measured on the finished unit.
2. **Module installation conditions** — the grant is only valid if the module is integrated per its instructions (approved antenna, spacing, shielding); the end-product maker is responsible for verifying this.

**Verified against a real competitor filing:** TRMNL (`2BFWO-OG`) — obviously built on a commodity WiFi module, same move as Ori — still submitted full assembled-device test reports (RF exposure, conducted power, radiated emissions). Nobody in this category ships a radio product on the strength of the chip's cert alone.

**Canada:** ISED's RSS-Gen (radio) + ICES-003 (digital device) mirror the US two-tier split; one accredited-lab test report can often support both US (FCC) and Canadian (ISED) filings, and CE for EU, rather than fully separate campaigns.

---

## 5. Open items before finalizing

- [ ] Get real quotes for the soft-estimate lines: enclosure filament/labor, packaging, hardware kit.
- [ ] Confirm assembly/QC labor rate against actual per-unit build time.
- [ ] Get a firm certification quote from an accredited lab (FCC + ISED bundle) — this dominates the NRE and the batch-one margin.
- [ ] Decide certification strategy (§3: R&D-amortized / larger batch / uncertified beta).
- [ ] Re-check FX before committing to USD purchase orders.
