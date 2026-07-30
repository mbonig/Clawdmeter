#pragma once

// Session-usage tracker: remembers recent session_pct samples so a refill of the
// 5-hour limit can be spotted. (It used to also bucket the *rate* of change into
// four tiers for the splash screen to pick animations from — that went away with
// the splash screen itself.)

// Feed in the latest session percentage every time fresh BLE data arrives.
// Returns true when this sample is a session reset (pct dropped substantially
// vs the previous sample) — the caller uses this to chime the buzzer. Never
// true on the first sample after boot (no prior sample to compare against).
bool usage_rate_sample(float session_pct);
