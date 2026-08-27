#include "backoff.h"

unsigned long nextBackoffDelayMs(unsigned long previousDelayMs, unsigned long startMs, unsigned long capMs) {
  if (previousDelayMs == 0) return startMs;
  unsigned long doubled = previousDelayMs * 2UL;
  return (doubled < capMs) ? doubled : capMs;
}

unsigned long detectorSafeBackoffCapMs(unsigned long globalCapMs, unsigned long detectorThresholdMs,
                                        unsigned long startMs) {
  unsigned long half = detectorThresholdMs / 2UL;
  unsigned long capped = (half < globalCapMs) ? half : globalCapMs;
  return (capped > startMs) ? capped : startMs;
}
