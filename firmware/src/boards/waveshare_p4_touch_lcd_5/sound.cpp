#include "../../hal/sound_hal.h"

// This board has an ES8311 codec + speaker terminal, but the audio path is
// unverified and out of scope for this port (see board.h) — same posture as
// the 2.06 board's un-wired chime path. No-op until wired up.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
