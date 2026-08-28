#include "backoff.h"

unsigned long nextBackoffDelayMs(unsigned long previousDelayMs, unsigned long startMs, unsigned long capMs) {
  if (previousDelayMs == 0) return startMs;
  unsigned long doubled = previousDelayMs * 2UL;
  // unsigned long is 32-bit on this platform - previousDelayMs above
  // ULONG_MAX/2 (~2.147e9ms, ~24.8 days) makes *2UL wrap to a SMALL
  // value, which could then read as "under capMs" and return the wrapped
  // (small) delay instead of staying clamped at the cap - the backoff
  // would suddenly shrink instead of holding steady. Not reachable by any
  // real caller today (every one keeps previousDelayMs far below this),
  // but this is a shared primitive future callers won't necessarily bound
  // as carefully - doubled < previousDelayMs is the standard unsigned-
  // overflow tell (a valid doubling can only grow, never shrink).
  if (doubled < previousDelayMs) return capMs;
  return (doubled < capMs) ? doubled : capMs;
}

unsigned long detectorSafeBackoffCapMs(unsigned long globalCapMs, unsigned long detectorThresholdMs,
                                        unsigned long startMs) {
  unsigned long half = detectorThresholdMs / 2UL;
  unsigned long capped = (half < globalCapMs) ? half : globalCapMs;
  return (capped > startMs) ? capped : startMs;
}
