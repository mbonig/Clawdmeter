#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // screen title / clock
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line
    const lv_font_t* anim_font;      // animated status line
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens

    // Media/volume control bar (BoardCaps.has_media_controls — P4 only, see
    // openspec/changes/add-media-volume-controls). Bottom-anchored off scr_h
    // rather than tied to content_y/usage_panel_h, since the only board that
    // sets the flag shares its breakpoint with boards tuned for a much
    // shorter panel (480 vs. 1280) — anchoring off the bottom edge keeps the
    // bar looking bottom-of-screen regardless of how much dead space sits
    // above it on a given board.
    int16_t control_bar_y;           // band top edge
    int16_t control_bar_h;           // band height
    int16_t control_btn_size;        // per-button width (touch target)
    int16_t control_btn_h;           // per-button height (taller: absorbs Y error)
    int16_t title_nudge;             // title x-shift balancing the corner logo
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery icon top edge
    int16_t batt_w;                  // battery icon width, for position math

    // Pairing hint / idle screen
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;

    // Round-display usage gauges (semicircle arcs instead of rectangular bars)
    bool     is_round;
    int16_t  round_cx, round_cy;   // arc center — screen center
    int16_t  round_r_out;          // arc outer radius
    int16_t  round_r_in;           // arc inner radius (round_r_out - round_arc_w) — divider length
    int16_t  round_arc_w;          // arc track thickness
    const lv_font_t* round_title_font;
    const lv_font_t* round_pct_font;
    const lv_font_t* round_pill_font;
    const lv_font_t* round_sub_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;
    L.is_round = c.is_round;

    if (c.is_round) {
        // Round panel — usage is two semicircle gauges (top = current, bottom =
        // weekly) sharing one ring, not rectangular bar panels. Geometry below
        // is tuned for 360x360 (Waveshare LCD-1.85B); new round ports may need
        // a fresh pass but inherit sane defaults via the same formulas.
        int16_t half = (L.scr_w < L.scr_h ? L.scr_w : L.scr_h) / 2;
        L.round_cx = L.scr_w / 2;
        L.round_cy = L.scr_h / 2;
        // Battery meter is hidden for now (see ui_init), so the ring doesn't need
        // to leave header room for it — runs almost edge-to-edge (3px margin,
        // just enough to keep the outer stroke from anti-aliasing off-panel).
        L.round_r_out = half - 3;
        L.round_arc_w = 80;          // 100% wider — was 40
        L.round_r_in  = L.round_r_out - L.round_arc_w;
        L.round_title_font  = &font_styrene_24;
        L.round_pct_font    = &font_styrene_28;   // smaller now the ring dominates — was 48
        L.round_pill_font   = &font_styrene_16;
        L.round_sub_font    = &font_styrene_14;

        // Fields below aren't used by the round usage builder but the shared
        // pairing-hint screen (build_pair_group) still reads the bt_* fonts.
        L.content_y = 0;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
        L.content_w = L.scr_w - 2 * L.margin;
        return;
    }

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        // Without this the bar is created at zero height and is simply invisible —
        // only the "small" branch below used to set it, so every board on the large
        // and compact layouts silently had no progress bar at all. 22px sits inside
        // the 38px gap between usage_bar_y and usage_reset_y.
        L.bar_h = 22;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
        // make_usage_panel()'s placeholder "---%"/"---" labels read these
        // directly — left unset here (and in "compact" below) until now, so
        // they were always null on every existing board. Rendering a style
        // with a null font pointer happened to be harmless on the Xtensa
        // boards that have shipped so far, but hangs LVGL's renderer outright
        // on ESP32-P4 (waveshare_p4_touch_lcd_5) — found while bringing that
        // board up. ui_update() immediately overwrites pct_font with
        // font_tiempos_56/font_styrene_48 once real data arrives (see the
        // enterprise branch below), so font_styrene_48 here just means the
        // brief pre-first-update placeholder matches the common (non-
        // enterprise) case instead of flashing a different font.
        L.pct_font   = &font_styrene_48;
        L.reset_font = &font_styrene_20;
        // Same class of bug as the null pct_font/reset_font above: idle_px was
        // never set on this breakpoint (or "compact" below), so splash_mini_create()
        // received px=0, floored its cell size to the 1px minimum, and rendered
        // the sleeping-creature idle screen as a 20x20px dot instead of a real
        // creature. Harmless-looking on every board that's shipped so far only
        // because the connected-but-no-data "idle" state is normally transient —
        // found here because the P4 sat in it long enough (mid BLE-reconnect
        // debugging) to actually look at it.
        L.idle_px = 160;
        if (c.has_media_controls) {
            // Sized generously on real-hardware evidence: two separate
            // deliberate "tap near the bottom" attempts landed hundreds of
            // pixels short (measured y=805 and y=1079 against an intended
            // target around y=1120-1210), while a top-of-screen calibration
            // tap landed pixel-perfect — ruling out an axis swap/scale bug
            // (GT911's own reported resolution is exactly 720x1280, matching
            // the panel) and pointing instead at plain human aiming variance
            // near an unfamiliar small target close to a screen edge
            // (likely compounded by touchscreen parallax). Bigger buttons
            // plus a generous dead-zone in global_click_cb (see there) are
            // the practical fix, not a coordinate transform.
            // Sized off the 720px width rather than fixed constants: 5 slots across
            // (720 - 2*20) / 5 = 136, with a 124px round button leaving a 12px gap
            // between neighbours. Now that touch coordinates are accurate there's no
            // reason to keep the old 100px bezel inset (that was a workaround for a
            // phantom X error — see touch.cpp), so the buttons get that space back.
            L.control_btn_size = 124;
            L.control_btn_h    = 124;   // circular
            L.control_bar_h    = 150;
            // Bottom-anchored off scr_h rather than tied to the panel offsets: this
            // breakpoint's absolute panel constants were tuned for a 480-tall screen
            // and this board is 1280 tall, so anchoring off the top would strand the
            // bar mid-screen with hundreds of pixels of dead space below it. Leaves
            // room for the independently bottom-anchored status line.
            L.control_bar_y = L.scr_h - L.control_bar_h - 70;
        }
    } else if (c.height >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bar_h = 18;   // see the large branch — this was also unset (invisible bar)
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
        // Same null-font-pointer fix as "large" above — see the comment there.
        L.pct_font   = &font_styrene_48;
        L.reset_font = &font_styrene_16;
        // Same idle_px fix as "large" above — see that comment.
        L.idle_px = 140;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, and
        // the corner logo/battery switch to the 40px/24px small assets.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        L.usage_panel_h = 74;
        L.usage_panel_gap = 6;
        L.usage_bar_y = 30;
        L.usage_reset_y = 46;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
        // Center the status line in the strip below the weekly panel; flush
        // against the bottom edge it reads as unevenly spaced.
        L.anim_y = -10;
        L.small_icons = true;
        L.title_nudge = 8;
        L.logo_y = 2;
        L.batt_y = 10;
        L.batt_w = ICON_BATTERY_SMALL_W;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_TEAL      THEME_TEAL
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Media/volume control bar (BoardCaps.has_media_controls) ----
static lv_image_dsc_t control_icon_dscs[5];  // vol down, mute, vol up, play/pause, next

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

// Sets fill + color on either a rectangular bar or a round-display arc gauge —
// the two share the same lv_obj_t* pointer slot (bar_session/bar_weekly), so
// callers don't need to know which one the active board built.
static void set_gauge(lv_obj_t* gauge, int pct, lv_color_t color) {
    if (L.is_round) {
        lv_arc_set_value(gauge, pct);
        lv_obj_set_style_arc_color(gauge, color, LV_PART_INDICATOR);
    } else {
        lv_bar_set_value(gauge, pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(gauge, color, LV_PART_INDICATOR);
    }
}

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    // Teal on every board. The rectangular layouts used to use COL_GREEN here while
    // only the round display used COL_TEAL — two different accent colours for the same
    // element with no real reason, so they're unified on the round display's teal.
    return COL_TEAL;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    // Initial fill colour before any data arrives; set_gauge() recolours per
    // percentage. Matches the round display's arc indicator (see make_gauge_arc).
    lv_obj_set_style_bg_color(bar, COL_TEAL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

// Round-display progress gauge: a semicircle arc (0..100 value range mapped
// onto [start_angle, end_angle]) instead of a rectangular bar. Two of these,
// covering the top and bottom halves respectively, form one ring split into
// independent current/weekly fills. Non-interactive — knob removed, not
// clickable — so it behaves like the bar it replaces (tap passes through to
// usage_container's splash-toggle handler).
static lv_obj_t* make_gauge_arc(lv_obj_t* parent, int32_t start_angle, int32_t end_angle) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 2 * L.round_r_out, 2 * L.round_r_out);
    lv_obj_set_pos(arc, L.round_cx - L.round_r_out, L.round_cy - L.round_r_out);
    lv_arc_set_bg_angles(arc, start_angle, end_angle);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_arc_width(arc, L.round_arc_w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, L.round_arc_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, COL_TEAL, LV_PART_INDICATOR);  // round-only helper
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}

// One gauge half: the arc plus its centered pct/label/reset text stack. Text
// is parented directly to `parent` (not the arc) and placed via LV_ALIGN_CENTER
// offsets so it sits in the safe inner disk the arc never draws into.
static lv_obj_t* make_usage_gauge_round(lv_obj_t* parent, int32_t angle_start, int32_t angle_end,
                                         int16_t y_off_caption, int16_t y_off_pct, int16_t y_off_reset,
                                         const char* pill_text,
                                         lv_obj_t** out_pct, lv_obj_t** out_pill, lv_obj_t** out_reset) {
    lv_obj_t* arc = make_gauge_arc(parent, angle_start, angle_end);

    *out_pct = lv_label_create(parent);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.round_pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_align(*out_pct, LV_ALIGN_CENTER, 0, y_off_pct);

    *out_pill = lv_label_create(parent);
    lv_label_set_text(*out_pill, pill_text);
    lv_obj_set_style_text_font(*out_pill, L.round_pill_font, 0);
    lv_obj_set_style_text_color(*out_pill, COL_DIM, 0);
    lv_obj_align(*out_pill, LV_ALIGN_CENTER, 0, y_off_caption);

    *out_reset = lv_label_create(parent);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.round_sub_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_align(*out_reset, LV_ALIGN_CENTER, 0, y_off_reset);

    return arc;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

static void init_battery_icons(void) {
    if (L.small_icons) {
        init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_SMALL_W, ICON_BATTERY_SMALL_H, icon_battery_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_SMALL_W, ICON_BATTERY_LOW_SMALL_H, icon_battery_low_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_SMALL_W, ICON_BATTERY_MEDIUM_SMALL_H, icon_battery_medium_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_SMALL_W, ICON_BATTERY_FULL_SMALL_H, icon_battery_full_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_SMALL_W, ICON_BATTERY_CHARGING_SMALL_H, icon_battery_charging_small_data);
        return;
    }
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// Order matches the on-screen left-to-right button order: Vol-, Mute, Vol+, Play/Pause, Next.
static const ble_consumer_key_t CONTROL_BAR_KEYS[5] = {
    BLE_CONSUMER_VOLUME_DOWN, BLE_CONSUMER_MUTE, BLE_CONSUMER_VOLUME_UP,
    BLE_CONSUMER_PLAY_PAUSE, BLE_CONSUMER_NEXT_TRACK,
};

static void init_control_bar_icons(void) {
    init_icon_dsc_rgb565a8(&control_icon_dscs[0], ICON_VOLUME_DOWN_W, ICON_VOLUME_DOWN_H, icon_volume_down_data);
    init_icon_dsc_rgb565a8(&control_icon_dscs[1], ICON_VOLUME_X_W, ICON_VOLUME_X_H, icon_volume_x_data);
    init_icon_dsc_rgb565a8(&control_icon_dscs[2], ICON_VOLUME_UP_W, ICON_VOLUME_UP_H, icon_volume_up_data);
    init_icon_dsc_rgb565a8(&control_icon_dscs[3], ICON_PLAY_PAUSE_W, ICON_PLAY_PAUSE_H, icon_play_pause_data);
    init_icon_dsc_rgb565a8(&control_icon_dscs[4], ICON_NEXT_TRACK_W, ICON_NEXT_TRACK_H, icon_next_track_data);
}

// Buttons are plain lv_obj_t (not lv_btn_create()), matching this file's existing
// hand-styled-panel convention. Deliberately NOT given LV_OBJ_FLAG_EVENT_BUBBLE, so a
// tap here never reaches usage_container's global_click_cb (splash/idle toggle).
static void control_btn_event_cb(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    ble_consumer_key_t key = (ble_consumer_key_t)(intptr_t)lv_obj_get_user_data(btn);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) ble_consumer_press(key);
    else                          ble_consumer_release();  // RELEASED or PRESS_LOST
}

static lv_obj_t* control_bar = nullptr;   // media/volume bar, owned by usage_container


static void build_control_bar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    control_bar = bar;
    lv_obj_set_size(bar, L.scr_w, L.control_bar_h);
    lv_obj_set_pos(bar, 0, L.control_bar_y);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Small cosmetic inset so the outer buttons aren't flush against the bezel. (An
    // earlier version used 100px here, blaming an unreliable GT911 X-axis near the
    // edges — that was the field-misread bug in this board's touch.cpp, since fixed,
    // so the margin is now purely cosmetic and the space goes to bigger buttons.)
    const int n = 5;
    const int16_t side_margin = 20;
    int16_t usable_w = L.scr_w - 2 * side_margin;
    int16_t slot_w = usable_w / n;
    for (int i = 0; i < n; i++) {
        lv_obj_t* btn = lv_obj_create(bar);
        int16_t btn_h = L.control_btn_h > 0 ? L.control_btn_h : L.control_btn_size;
        lv_obj_set_size(btn, L.control_btn_size, btn_h);
        lv_obj_set_pos(btn, side_margin + slot_w * i + (slot_w - L.control_btn_size) / 2,
                        (L.control_bar_h - btn_h) / 2);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, COL_BAR_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)CONTROL_BAR_KEYS[i]);
        lv_obj_add_event_cb(btn, control_btn_event_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(btn, control_btn_event_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(btn, control_btn_event_cb, LV_EVENT_PRESS_LOST, NULL);

        lv_obj_t* icon = lv_image_create(btn);
        lv_image_set_src(icon, &control_icon_dscs[i]);
        // The source icons are 48px; scale them to about half the button so they stay
        // proportionate as the button size changes (LVGL zoom: 256 = 1:1, and it zooms
        // around the image's own centre, so lv_obj_center() below is unaffected).
        int32_t icon_target = L.control_btn_size / 2;
        if (icon_target > ICON_VOLUME_UP_W) {
            lv_image_set_scale(icon, (256 * icon_target) / ICON_VOLUME_UP_W);
        }
        lv_obj_center(icon);
    }
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text, &font_styrene_28);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Round panels are widest near the vertical center and narrow toward the
    // top edge, so the three lines sit lower (and closer together) than on a
    // rectangular panel to stay clear of the circular bezel.
    int16_t y1 = L.is_round ? 90  : 40;
    int16_t y2 = L.is_round ? 160 : 120;
    int16_t y3 = L.is_round ? 200 : 160;

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "expression sleep", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    if (L.is_round) {
        // Round header has room for the battery meter or a title, not both —
        // the meter wins. Kept alive (not destroyed) so the clock-text code in
        // ui_tick_anim() still has a valid target; it just never renders.
        lv_obj_set_style_text_font(lbl_title, L.round_title_font, 0);
        lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
        lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);
    }

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (L.is_round) {
        // Top semicircle (9 o'clock -> 12 o'clock -> 3 o'clock) = current.
        panel_session = make_usage_gauge_round(usage_group, 180, 360,
                         /*caption*/ -86, /*pct*/ -58, /*reset*/ -34, "Current",
                         &lbl_session_pct, &lbl_session_label, &lbl_session_reset);
        bar_session = panel_session;

        // Enterprise-only overlays — hidden until enterprise data arrives. Parented
        // to usage_group (not the arc) and pinned to the same slot as the reset/
        // caption labels they stand in for.
        lbl_session_pct_sym = lv_label_create(usage_group);
        lv_label_set_text(lbl_session_pct_sym, "%");
        lv_obj_set_style_text_font(lbl_session_pct_sym, &font_styrene_16, 0);
        lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

        lbl_spending_desc = lv_label_create(usage_group);
        lv_label_set_text(lbl_spending_desc, "of your monthly budget");
        lv_obj_set_style_text_font(lbl_spending_desc, L.round_sub_font, 0);
        lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
        lv_obj_align(lbl_spending_desc, LV_ALIGN_CENTER, 0, -14);
        lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

        lbl_spending_status = lv_label_create(usage_group);
        lv_label_set_text(lbl_spending_status, "");
        lv_obj_set_style_text_font(lbl_spending_status, L.round_sub_font, 0);
        lv_obj_align(lbl_spending_status, LV_ALIGN_CENTER, 0, 4);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

        // Bottom semicircle (3 o'clock -> 6 o'clock -> 9 o'clock) = weekly.
        panel_weekly = make_usage_gauge_round(usage_group, 0, 180,
                         /*caption*/ 86, /*pct*/ 58, /*reset*/ 34, "Weekly",
                         &lbl_weekly_pct, &lbl_weekly_label, &lbl_weekly_reset);
        bar_weekly = panel_weekly;

        // Two dividers (not one) flanking the center gap, created last so they
        // draw on top of both arcs. The gap between them houses the animated
        // status line (see below) instead of the two gauges reading as one blob.
        const int16_t rule_ys[2] = {-18, 18};
        for (int16_t rule_y : rule_ys) {
            lv_obj_t* divider = lv_obj_create(usage_group);
            lv_obj_set_size(divider, 2 * L.round_r_out, 3);
            lv_obj_set_pos(divider, L.round_cx - L.round_r_out, L.round_cy + rule_y - 1);
            lv_obj_set_style_bg_color(divider, COL_DIM, 0);
            lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(divider, 0, 0);
            lv_obj_set_style_radius(divider, 0, 0);
            lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(divider, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
    } else {
        panel_session = make_usage_panel(usage_group, L.content_y, "Current",
                         &lbl_session_pct, &lbl_session_label,
                         &bar_session, &lbl_session_reset);

        // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
        lbl_session_pct_sym = lv_label_create(panel_session);
        lv_label_set_text(lbl_session_pct_sym, "%");
        lv_obj_set_style_text_font(lbl_session_pct_sym, &font_styrene_28, 0);
        lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

        lbl_spending_desc = lv_label_create(panel_session);
        lv_label_set_text(lbl_spending_desc, "of your monthly budget");
        lv_obj_set_style_text_font(lbl_spending_desc, &font_styrene_28, 0);
        lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
        lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
        lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

        lbl_spending_status = lv_label_create(panel_session);
        lv_label_set_text(lbl_spending_status, "");
        lv_obj_set_style_text_font(lbl_spending_status, &font_styrene_16, 0);
        lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

        panel_weekly = make_usage_panel(usage_group,
                         L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                         &lbl_weekly_pct, &lbl_weekly_label,
                         &bar_weekly, &lbl_weekly_reset);

        // Parented to usage_container, NOT usage_group: the media/volume controls have
        // nothing to do with usage data, but usage_group is hidden whenever data goes
        // stale — which made the buttons disappear behind the idle "Listening…" screen
        // exactly when you'd still want to change the volume. update_view_state() hides
        // this only when BLE is down (the pairing view), where presses can't work anyway.
        if (board_caps().has_media_controls) build_control_bar(usage_container);
    }
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // pair_group/idle_group are created after the control bar and are both full-width
    // panels covering everything below the header, so in LVGL's create-order z-stacking
    // they sit ON TOP of the bar and silently swallow every tap aimed at it (the press
    // hit idle_group, bubbled to usage_container, and global_click_cb's dead zone then
    // dropped it — so nothing happened at all). Lift the bar back to the front so its
    // buttons are actually hittable on the idle screen.
    if (control_bar) lv_obj_move_foreground(control_bar);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    if (L.is_round) {
        // Small mono font (has the spinner glyphs; styrene doesn't). Dead center,
        // in the gap between the two divider rules — not at the bottom edge,
        // which the much-thicker ring/fill leaves almost no clearance against.
        lv_obj_set_style_text_font(lbl_anim, &font_mono_18, 0);
        lv_obj_align(lbl_anim, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
        lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
    }

}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, LOGO_SMALL_WIDTH, LOGO_SMALL_HEIGHT, logo_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();
    if (board_caps().has_media_controls) init_control_bar_icons();

    init_usage_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    if (L.is_round) {
        // No room for an 80px decorative logo in the round header — the
        // battery meter lives there instead (replacing the title text too).
        lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);
    }

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    if (L.is_round) {
        // Dead-center top, where the title used to be. Scaled down to ~2/3 size
        // (LVGL zooms around the image's own center, so the logical 48x48 box
        // stays centered on round_cx even though the visible pixels shrink).
        // Hidden for now — the ring runs edge-to-edge and there's no header
        // band left for it — but positioned so it's ready to re-enable.
        lv_image_set_scale(battery_img, 171);
        lv_obj_set_pos(battery_img, L.round_cx - 24, 22);
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);
    }
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    last_data_ms = lv_tick_get();   // a valid usage update just landed → dot goes green
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.is_round ? L.round_pct_font : &font_tiempos_56, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.is_round ? L.round_pct_font : &font_styrene_48, 0);
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    set_gauge(bar_session, s_pct, pct_color(data->session_pct));

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_TEAL :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        set_gauge(bar_weekly, data->time_pct, bar_pace);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        set_gauge(bar_weekly, w_pct, pct_color(data->weekly_pct));
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
    // Media controls stay usable whenever there's a link (live usage OR idle/stale
    // data); only the pairing view hides them, since nothing can be sent with BLE down.
    if (control_bar) {
        if (v == 0) lv_obj_add_flag(control_bar, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(control_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        if (tmv.tm_min != clock_last_min) {   // only rewrite the title when the minute changes
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    // Round boards keep it hidden regardless of screen — see ui_init().
    if (L.is_round) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    // Ignore taps inside the media/volume control-bar band, so a near-miss on one of
    // its buttons doesn't fall through to the "tap anywhere toggles splash" gesture
    // and yank the screen away. Checked by raw coordinate rather than by which widget
    // received the click, so it holds regardless of the hit-test path a near-miss
    // takes through the widget tree.
    //
    // The band is the bar's own bounds plus a small margin. It used to be 300px,
    // compensating for taps that appeared to land 150-300px short — but that was the
    // GT911 field-misread bug (see boards/waveshare_p4_touch_lcd_5/touch.cpp), not
    // real aiming error. Coordinates are accurate now, so the huge dead zone is
    // unnecessary and only made a large part of the screen mysteriously inert.
    if (board_caps().has_media_controls && L.control_bar_h > 0) {
        lv_indev_t* indev = lv_indev_active();
        if (indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            if (p.y >= L.control_bar_y - 20) return;
        }
    }
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    // Round boards never show the logo (no room for it in the header) —
    // ui_init() hides it permanently, so leave it alone here.
    if (logo_img && !L.is_round) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    if (!battery_img) return;
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
