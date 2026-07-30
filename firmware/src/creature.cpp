#include "creature.h"
#include "splash_animations.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// The scraped art is a 20x20 grid of palette indices per frame.
#define GRID      20
#define COL_EMPTY 0x0000   // true black (matches THEME_BG)

static lv_obj_t  *creature_canvas = NULL;
static uint16_t  *creature_buf = NULL;
static int        creature_cell = 0;
static int        creature_w = 0;
static const splash_anim_def_t *creature_anim = NULL;
static uint16_t   creature_frame = 0;
static uint32_t   creature_started = 0;

static void creature_render(void) {
    if (!creature_buf || !creature_anim) return;
    const uint8_t *cells = creature_anim->frames[creature_frame];
    const uint16_t *pal = creature_anim->palette;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (pal && code < SPLASH_PALETTE_SIZE) ? pal[code] : COL_EMPTY;
            for (int dy = 0; dy < creature_cell; dy++) {
                uint16_t *dst = &creature_buf[(gy * creature_cell + dy) * creature_w + gx * creature_cell];
                for (int dx = 0; dx < creature_cell; dx++) dst[dx] = color;
            }
        }
    }
    if (creature_canvas) lv_obj_invalidate(creature_canvas);
}

lv_obj_t* creature_create(lv_obj_t *parent, const char *anim_name, int px) {
    creature_anim = NULL;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        if (strcmp(splash_anims[i].name, anim_name) == 0) { creature_anim = &splash_anims[i]; break; }
    }
    if (!creature_anim) return NULL;
    creature_cell = px / GRID;
    if (creature_cell < 1) creature_cell = 1;
    creature_w = GRID * creature_cell;
#ifdef BOARD_HAS_PSRAM
    const uint32_t caps = MALLOC_CAP_SPIRAM;
#else
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    creature_buf = (uint16_t*)heap_caps_malloc(creature_w * creature_w * 2, caps);
    if (!creature_buf) return NULL;
    creature_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(creature_canvas, creature_buf, creature_w, creature_w, LV_COLOR_FORMAT_RGB565);
    creature_frame = 0;
    creature_started = millis();
    creature_render();
    return creature_canvas;
}

void creature_tick(void) {
    if (!creature_buf || !creature_anim || creature_anim->frame_count == 0) return;
    if (millis() - creature_started < creature_anim->holds[creature_frame]) return;
    creature_started = millis();
    creature_frame = (creature_frame + 1) % creature_anim->frame_count;
    creature_render();
}
