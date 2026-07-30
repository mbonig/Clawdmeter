#pragma once
#include <lvgl.h>

// Small animated pixel-art creature, embeddable in any screen. Uses the same
// scraped claudepix 20x20 art as the (now removed) full-screen splash — today
// the only user is the idle "Zzz" indicator on the usage screen.
//
// One creature at a time: creating a second one retargets the shared canvas.
// `px` is the on-screen size; the cell size is floor(px / 20), so a px that
// isn't a multiple of 20 renders slightly smaller rather than blurring.
lv_obj_t* creature_create(lv_obj_t *parent, const char *anim_name, int px);

// Advance the current creature's animation. Call from the UI tick while the
// creature is visible.
void creature_tick(void);
