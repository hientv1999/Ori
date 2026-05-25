# Ori — LVGL desktop simulator

Renders every Ori screen to a 800×480 BMP file without needing any hardware.

The simulator reuses the **same** LVGL-pure source files the firmware compiles:

```
firmware/src/theme.cpp
firmware/src/mock_data.cpp
firmware/src/widgets/*.cpp
firmware/src/screens/*.cpp
firmware/include/lv_conf.h          (shared config)
firmware/.pio/libdeps/ori/lvgl/...  (LVGL source PlatformIO already cached)
```

Hardware-specific files (`backlight`, `touch_gt911`, `nvs_store`, `io_expander_ch422g`, `lcd_panel`, `lvgl_display`, `lvgl_input`, the firmware's `main.cpp`) are **not** built into the simulator. They depend on Arduino headers.

The simulator's own files live only inside `sim/` and do not pollute the PlatformIO project. PlatformIO never sees this directory.

## Prerequisites

- **MinGW gcc / g++ on PATH** — CodeBlocks installs a working one at `C:\Program Files\CodeBlocks\MinGW\bin\`.
- **PlatformIO has already cached LVGL** — i.e. you have run `pio run -d firmware` at least once. The simulator pulls LVGL source from `firmware/.pio/libdeps/ori/lvgl/`.

## Build + run

```bash
cd sim
mingw32-make run
```

Output: `sim/screenshots/01..21_*.bmp`, one per Ori screen / variant.

## Clean

```bash
mingw32-make clean
```

## What the simulator is NOT

- It is **not** interactive. Mouse / keyboard input is not wired. Each invocation re-renders all screens fresh.
- It does **not** match hardware pixel-for-pixel — LVGL renders identically on both sides, but Windows BMP viewers gamma-correct slightly differently than the LCD panel.
- It does **not** simulate touch gestures, BLE, or NVS persistence. Those live in firmware-only modules.

If you need interactive simulation later, add SDL2 + a small mouse/keyboard event loop in `main.cpp` — the LVGL display driver setup is already there.
