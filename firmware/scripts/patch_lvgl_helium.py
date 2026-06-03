"""
patch_lvgl_helium.py — PlatformIO pre-build extra_script
=========================================================
Replaces lv_blend_helium.S with a no-op stub before any build tasks run.

Why: LVGL 9.5.0 ships ARM Helium SIMD assembly in that file. The Xtensa
ESP32-S3 assembler cannot parse it because the file's transitive C-header
includes (via lv_conf_internal.h) contain typedef statements the assembler
rejects. Swapping the file for an empty .note.GNU-stack section is the only
reliable workaround.

Code at module level runs during SCons configuration — before any compile
tasks are scheduled — so the stub is in place before the assembler ever
sees the file. This survives branch switches that reinstall the libdeps.
"""

import os

Import("env")  # noqa: F821 — injected by SCons/PlatformIO

STUB = """\
/* lv_blend_helium.S — no-op stub for Xtensa ESP32-S3.
 * ARM Helium SIMD is not available on this target; the real implementation
 * is guarded by __ARM_FEATURE_MVE which is never set for Xtensa.
 * Patched automatically by scripts/patch_lvgl_helium.py before every build.
 */
#ifndef __ASSEMBLY__
#define __ASSEMBLY__
#endif
#ifdef __ELF__
.section .note.GNU-stack,"",%progbits
#endif
"""

libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
stub_path = os.path.join(
    libdeps, "ori", "lvgl",
    "src", "draw", "sw", "blend", "helium",
    "lv_blend_helium.S",
)

if os.path.isfile(stub_path):
    current = open(stub_path, "r", encoding="utf-8").read()
    if "no-op stub" not in current:
        with open(stub_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(STUB)
        print("[patch_helium] lv_blend_helium.S patched for Xtensa toolchain")
    else:
        print("[patch_helium] lv_blend_helium.S already patched — skipping")
else:
    print(f"[patch_helium] {stub_path} not found — skipping (LVGL 8 or path changed?)")
