# Ori — Project Memory

Stable facts with no better home. Only update if a fundamental decision changes.

---

## Names & URLs

| Item | Value |
|---|---|
| Product name | **Ori** |
| PC companion app name | **Orion** |
| Orion installation URL | `ori.app/orion` |
| Orion supported platforms | Windows, macOS |
| BLE device name format | `Ori-XX-XX` (per-device suffix, e.g. `Ori-XT-9F`) |

---

## Library Versions

| Library | Pinned version |
|---|---|
| LVGL | **9.5.0** — `lvgl/lvgl@9.5.0` in `firmware/platformio.ini` |
| NimBLE-Arduino | **2.5.0** — `h2zero/NimBLE-Arduino@2.5.0` (add to `lib_deps` at M5) |

---

## Fixed Numeric Constants

| Constant | Value |
|---|---|
| Pre-meeting alert | 5 minutes before start |
| Long-press duration | 3 seconds (factory reset + re-pair phone) |
| Profile photo size | 228 × 228 px JPEG |

---

## Wordmark

Lowercase `ori` text; "o" and "i" in primary text colour, "r" in accent gold `#E0B86A` (`theme::COLOR_ACCENT`). Flanking gradient lines on setup screens. Used as the album-art empty-state placeholder (centred on dark gradient) and the Orion app icon.
