#pragma once
#include <time.h>

// Whether the clock has been set: until the first sync it counts up from 1970.
inline bool clockYearIsSet(int tmYear) { return tmYear > (2016 - 1900); }
inline bool clockIsSet(time_t now) { return now >= 1483228800; } // 2017-01-01 UTC

// Whether to resolve the NTP server again and restart SNTP, tested natively.
// At most once per retryMs. Restarts if SNTP isn't running (the lookup failed)
// or hasn't synced since it started, or if it has missed three sync intervals
// in a row (the server's address may have changed).
bool shouldRestartSntp(bool started, bool syncedSinceStart, unsigned long msSinceSync, unsigned long msSinceAttempt,
                       unsigned long syncIntervalMs, unsigned long retryMs);
