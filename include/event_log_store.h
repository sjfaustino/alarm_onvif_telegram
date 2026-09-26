#pragma once
#include <Arduino.h>
#include <vector>
#include "event_log.h" // EventLogEntry, EventRingBuffer - the pure ring buffer (lib/event_log)

// Thread-safe global Activity log: a mutex around lib/event_log's ring.
//
// Not named event_log.h: two same-named headers on the include path once
// resolved differently depending on flag order.

// In-RAM, recent activity only. Exposed so the Activity page's hint text
// matches.
static const size_t EVENT_LOG_CAPACITY = 40;

// Appends with the current millis() timestamp.
void logEvent(const String& text);

// Oldest-first copy, so the caller doesn't hold the lock while rendering.
std::vector<EventLogEntry> recentEvents();
