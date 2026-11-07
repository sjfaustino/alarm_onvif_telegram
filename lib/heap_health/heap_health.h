#pragma once
#include <stdint.h>

// Pure logic behind checkHeapHealth, tested natively. ESP.getMinFreeHeap() is
// a lifetime low-water mark: each new record is logged, and the first one
// below HEAP_LOW_WARN_BYTES alerts once.
struct HeapHealthResult {
  uint32_t baseline;   // caller should remember this for the next call's previousBaseline
  bool shouldLog;       // true if the caller should write an Activity log entry
  bool isNewLow;        // true if shouldLog is because of an actual drop - false only for the
                         // first observation this boot (a baseline, not a
                         // drop)
  bool shouldAlert;      // true only the first time a record low crosses below warnBytes
};

// hasBaseline/previousBaseline: the caller's state from the last call. The
// minimum never rises within a boot, so the alert never re-arms.
// Whether to write a pending heap-low record to NVS now: rate-limited to limit
// flash wear, but the newest record still lands once the gap has passed.
bool shouldPersistHeapLow(bool pending, unsigned long msSinceLastSave, unsigned long minGapMs);

HeapHealthResult evaluateHeapHealth(uint32_t minFreeHeapNow, bool hasBaseline, uint32_t previousBaseline,
                                     uint32_t warnBytes, bool alreadyAlerted);
