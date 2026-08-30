#pragma once
#include <Arduino.h>
#include <vector>

// Pure fixed-capacity ring buffer of "what happened recently" - split out
// of the thread-safe global wrapper (include/event_log_store.h,
// src/event_log_store.cpp) so the actual eviction logic is unit-testable
// without FreeRTOS. Not a persistent log: NVS wear/size would make writing
// one entry per motion alert/on-off/reboot impractical, so this is purely
// in-RAM and resets on reboot - "what's happened since the board last came
// up," not a permanent history. See src/event_log_store.cpp for the global
// logEvent()/recentEvents() every other file actually calls.

struct EventLogEntry {
  unsigned long ms; // millis() timestamp when logged
  String text;
};

class EventRingBuffer {
 public:
  explicit EventRingBuffer(size_t capacity) : capacity_(capacity) {}

  // Appends one entry; once size() would exceed capacity, the single
  // oldest entry is dropped first (not the whole buffer) - a burst of
  // events (e.g. several cameras going offline in the same WiFi outage)
  // doesn't wipe out everything older than the burst, only trims the very
  // oldest entries to make room.
  void push(unsigned long ms, const String& text);

  size_t size() const { return entries_.size(); }
  size_t capacity() const { return capacity_; }

  // Oldest-first, same order push() was called in.
  const std::vector<EventLogEntry>& entries() const { return entries_; }

 private:
  size_t capacity_;
  std::vector<EventLogEntry> entries_;
};
