#include "heap_health.h"

HeapHealthResult evaluateHeapHealth(uint32_t minFreeHeapNow, bool hasBaseline, uint32_t previousBaseline,
                                     uint32_t warnBytes, bool alreadyAlerted) {
  HeapHealthResult r;

  if (!hasBaseline) {
    // First observation this boot - a reference point, not a "drop" (it may
    // already reflect whatever setup()/boot itself needed) - still worth
    // one Activity log line so there's at least one timestamped data point,
    // instead of /health's "min ever" being the only place this number
    // ever shows up.
    r.baseline = minFreeHeapNow;
    r.shouldLog = true;
    r.isNewLow = false;
    r.shouldAlert = !alreadyAlerted && minFreeHeapNow < warnBytes;
    return r;
  }

  if (minFreeHeapNow >= previousBaseline) {
    // No new record low since the last check - nothing to report.
    r.baseline = previousBaseline;
    r.shouldLog = false;
    r.isNewLow = false;
    r.shouldAlert = false;
    return r;
  }

  r.baseline = minFreeHeapNow;
  r.shouldLog = true;
  r.isNewLow = true;
  r.shouldAlert = !alreadyAlerted && minFreeHeapNow < warnBytes;
  return r;
}
