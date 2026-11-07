#pragma once
#include <Arduino.h>

// Remote diagnostics for a board with no serial cable: failed allocations and a
// heap-low record that survives a reboot. Served by the /debug routes.

// Call early in setup().
void initDiagnostics();

// Call every loop() tick with the current lifetime minimum, before anything that
// might allocate much.
void diagnosticsOnHeapTick(uint32_t minFreeHeap, bool isNewLow);

struct AllocFailure {
  uint32_t size;
  uint32_t caps;
  uint32_t atMs;
  char task[16];
  const char* function;
};
// Copies the most recent failures, newest first. Returns how many were copied;
// *total counts every failure since boot.
size_t recentAllocFailures(AllocFailure* out, size_t maxCount, uint32_t* total);

// The saved heap-low record as JSON, "" if none. It may be from an earlier boot.
String loadHeapLowRecord();
