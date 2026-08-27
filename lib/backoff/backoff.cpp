#include "backoff.h"

unsigned long nextBackoffDelayMs(unsigned long previousDelayMs, unsigned long startMs, unsigned long capMs) {
  if (previousDelayMs == 0) return startMs;
  unsigned long doubled = previousDelayMs * 2UL;
  return (doubled < capMs) ? doubled : capMs;
}
