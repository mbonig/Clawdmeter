#pragma once
#include <Arduino.h>

// Market quotes for the center rotator (see ui.cpp). The daemon pre-formats the
// price string so currency/precision decisions stay host-side; only the percent
// change comes across as a number, because the firmware colors it by sign.
#define MAX_QUOTES 3

struct QuoteData {
    char  sym[10];    // ticker as it should be displayed, e.g. "AMZN"
    char  price[14];  // formatted by the daemon, e.g. "$212.34"
    float chg_pct;    // percent change vs. previous close
    bool  has_chg;    // false when the daemon couldn't compute a change
};

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    QuoteData quotes[MAX_QUOTES];  // market quotes from the daemon (may be empty)
    int  quote_count;        // number of populated entries in quotes[]
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
