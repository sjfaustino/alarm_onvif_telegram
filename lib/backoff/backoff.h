#pragma once

// Doubling backoff with a cap, shared by WiFi reconnect and camera
// resubscribe. previousDelayMs 0 means the first failure in a streak (callers
// reset to 0 on success).
unsigned long nextBackoffDelayMs(unsigned long previousDelayMs, unsigned long startMs, unsigned long capMs);

// Keeps a retry backoff below a "gone quiet" detector's threshold. With both
// at 5 minutes, a camera between retries false-alarmed as offline.
//
// Returns half of detectorThresholdMs, clamped to [startMs, globalCapMs].
unsigned long detectorSafeBackoffCapMs(unsigned long globalCapMs, unsigned long detectorThresholdMs,
                                        unsigned long startMs);
