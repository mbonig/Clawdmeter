#pragma once
#include <lvgl.h>

// Design tokens — single source of truth for UI colors. Anthropic-inspired
// dark palette, AMOLED-friendly (true black bg).
#define THEME_BG       lv_color_hex(0x000000)   // screen background
#define THEME_PANEL    lv_color_hex(0x1f1f1e)   // card/zone fill
#define THEME_TEXT     lv_color_hex(0xfaf9f5)   // primary text
#define THEME_DIM      lv_color_hex(0xb0aea5)   // secondary text
// Terra-cotta, brightened one step from Anthropic's brand #d97757 (same ~15° hue,
// saturation 60%->68%, value 85%->91%) because the brand value reads as drab against
// the true-black background. KEEP IN SYNC with two other places that must use the
// identical value: the splash palette remap in tools/convert_to_c.js (the pixel-art
// creature is recoloured to this so it matches the UI), and the `pace_hex` recolor
// string in ui.cpp (LVGL label recoloring needs a literal hex, not a token).
#define THEME_ACCENT   lv_color_hex(0xe8734a)   // brand terra-cotta, brightened
#define THEME_GREEN    lv_color_hex(0x788c5d)
// Under-threshold gauge/bar fill on EVERY board (round arcs and rectangular bars
// alike — see pct_color()). Brightened from the original 0x4a9a94, whose 60% value
// made it recede against the dark panel: same ~175° hue, saturation 52%->60%,
// value 60%->75%.
#define THEME_TEAL     lv_color_hex(0x4cbfb6)
#define THEME_AMBER    lv_color_hex(0xe8734a)
#define THEME_RED      lv_color_hex(0xc0392b)
#define THEME_BAR_BG   lv_color_hex(0x2a2a28)   // unfilled bar track
