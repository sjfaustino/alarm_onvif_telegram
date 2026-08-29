#pragma once

// Simple doubling backoff with a cap - shared by main.cpp's WiFi reconnect
// and camera.cpp's per-camera subscription retry (each used to hand-write
// this formula separately) so both stay in sync and it's unit-tested once
// (test/test_backoff).
//
// previousDelayMs: the delay used on the previous attempt, or 0 for the
// first failure in a streak (both callers reset to 0 on success, making 0
// an unambiguous "start over"). startMs: delay for that first failure.
// capMs: the ceiling, however long the streak runs.
unsigned long nextBackoffDelayMs(unsigned long previousDelayMs, unsigned long startMs, unsigned long capMs);

// Caps a backoff so it can't grow slower than a separate "gone quiet for
// this long" detector. Real incident: camera.cpp's subscription-retry
// backoff and its offline-threshold alert were independently set to the
// same 5 minutes - a camera whose backoff reached that ceiling could go
// quiet between retries just long enough to false-alarm, since a SOAP
// fault still counts as "contact" there but only refreshes on each retry.
//
// Returns half of detectorThresholdMs, clamped to [startMs, globalCapMs].
unsigned long detectorSafeBackoffCapMs(unsigned long globalCapMs, unsigned long detectorThresholdMs,
                                        unsigned long startMs);
