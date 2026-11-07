#pragma once

// Whether to resolve the NTP server again and restart SNTP, tested natively.
// At most once per retryMs. Restarts if SNTP isn't running (the lookup failed)
// or hasn't synced since it started, or if it has missed three sync intervals
// in a row (the server's address may have changed).
bool shouldRestartSntp(bool started, bool syncedSinceStart, unsigned long msSinceSync, unsigned long msSinceAttempt,
                       unsigned long syncIntervalMs, unsigned long retryMs);
