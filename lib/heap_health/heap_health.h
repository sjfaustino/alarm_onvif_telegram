#pragma once
#include <stdint.h>

// Pure decision logic behind checkHeapHealth (main.cpp) - split out so it's
// natively unit-tested (test/test_heap_health) without FreeRTOS/Arduino,
// same reasoning as background_job_state.h/backoff.h/subscription_health.h.
//
// ESP.getMinFreeHeap() (internal SRAM, not PSRAM) is a running lifetime-low
// watermark the IDF already tracks continuously and cheaply - checkHeapHealth
// polls it and, whenever it's dropped to a NEW record low since the last
// poll, logs a timestamped Activity-log entry (so a slow leak or a burst
// event leaves a trail that can be correlated against other events around
// the same time), and sends a one-time Telegram alert the first time a
// record low crosses HEAP_LOW_WARN_BYTES (config.h) - real allocation-
// failure territory, not a sanity nicety, since mbedTLS/WiFiClientSecure
// allocate from this same pool.
struct HeapHealthResult {
  uint32_t baseline;   // caller should remember this for the next call's previousBaseline
  bool shouldLog;       // true if the caller should write an Activity log entry
  bool isNewLow;        // true if shouldLog is because of an actual drop - false only for the
                         // very first observation this boot (a baseline reference point, not a "drop")
  bool shouldAlert;      // true only the first time a record low crosses below warnBytes
};

// hasBaseline/previousBaseline: the caller's own state from the last call
// (false/anything on the very first call this boot). warnBytes:
// HEAP_LOW_WARN_BYTES. alreadyAlerted: whether shouldAlert has already
// fired once this boot - since minFreeHeapNow is a running minimum that
// never increases within a boot, this never needs to re-arm.
HeapHealthResult evaluateHeapHealth(uint32_t minFreeHeapNow, bool hasBaseline, uint32_t previousBaseline,
                                     uint32_t warnBytes, bool alreadyAlerted);
