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
