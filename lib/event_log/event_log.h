#pragma once
#include <Arduino.h>
#include <vector>

// Pure fixed-capacity ring of recent events, tested natively; the global
// thread-safe wrapper is event_log_store.h. In-RAM only.

struct EventLogEntry {
  unsigned long ms; // millis() timestamp when logged
  String text;
};

class EventRingBuffer {
 public:
  explicit EventRingBuffer(size_t capacity) : capacity_(capacity) {}

  // When full, drops only the oldest entry.
  void push(unsigned long ms, const String& text);

  size_t size() const { return entries_.size(); }
  size_t capacity() const { return capacity_; }

  const std::vector<EventLogEntry>& entries() const { return entries_; }

 private:
  size_t capacity_;
  std::vector<EventLogEntry> entries_;
};
