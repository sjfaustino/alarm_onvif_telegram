#pragma once

// Simple doubling backoff with a cap. main.cpp's top-level WiFi reconnect
// and camera.cpp's per-camera ONVIF subscription retry each independently
// hand-wrote this exact formula - shared here instead, so it can be
// unit-tested once (test/test_backoff) and the two call sites can't drift
// out of sync with each other.
//
// previousDelayMs: the delay used for the *previous* attempt in the
// current failure streak, or 0 if there is no previous attempt (either
// this is the first failure since the last success, or nothing has
// failed yet - both callers reset their stored delay to 0 on success,
// which is what makes 0 an unambiguous "start over" signal here).
// startMs: the delay to use for that first failure. capMs: the maximum
// delay this will ever return, however long the failure streak runs.
unsigned long nextBackoffDelayMs(unsigned long previousDelayMs, unsigned long startMs, unsigned long capMs);

// A backoff cap that also can't grow slower than a separate "went quiet
// for this long" detector - camera.cpp's per-camera subscription retry
// backoff vs. its offline-threshold alert, specifically. A SOAP fault
// still counts as "contact" there, so a camera stuck failing retries
// keeps refreshing its last-contact time on every attempt, but only as
// often as it's actually retried; if the retry cadence alone drifted
// slower than the offline threshold, the gap between retries could trip
// a false offline alert for a camera that's still there and about to
// answer again. Hit in the field: both were independently set to 5
// minutes (one a global constant, one a per-camera default), so a camera
// whose backoff reached its ceiling could go quiet just long enough to
// false-alarm.
//
// Returns half of detectorThresholdMs, clamped to [startMs, globalCapMs] -
// floored at startMs so a very short configured threshold can't push the
// result below backoff's own first-attempt delay.
unsigned long detectorSafeBackoffCapMs(unsigned long globalCapMs, unsigned long detectorThresholdMs,
                                        unsigned long startMs);
