#include "usage_rate.h"

// Only the previous sample matters now that the rate tiers are gone: a reset is
// a substantial drop against it. The 5% threshold keeps API rounding noise (and
// a re-poll landing mid-percent) from reading as a refill.
#define RESET_DROP_PCT 5.0f

static bool  have_prev = false;
static float prev_pct  = 0.0f;

bool usage_rate_sample(float session_pct) {
    bool was_reset = have_prev && (session_pct + RESET_DROP_PCT < prev_pct);
    prev_pct = session_pct;
    have_prev = true;
    return was_reset;
}
