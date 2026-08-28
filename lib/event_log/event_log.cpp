#include "event_log.h"

void EventRingBuffer::push(unsigned long ms, const String& text) {
  entries_.push_back({ms, text});
  if (entries_.size() > capacity_) entries_.erase(entries_.begin());
}
