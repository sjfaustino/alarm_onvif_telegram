#pragma once
#include <Arduino.h>
#include <vector>
#include "event_log.h" // EventLogEntry, EventRingBuffer - the pure ring buffer (lib/event_log)

// Thread-safe global event log - append from any task, read from any task
// (PsychicHttp's dashboard render). A FreeRTOS-mutex wrapper around
// EventRingBuffer (lib/event_log, unit-tested); this glue layer isn't -
// see test/README.md.
//
// Named event_log_store.h, not event_log.h, so it can't collide with
// lib/event_log/event_log.h on the include path - two same-named headers
// reachable from one translation unit resolve to whichever the compiler's
// search order favors, silently and differently by flag order (a real bug
// here before - see platformio.ini's build_src_flags comment).

// Recent-activity view, not a permanent log - purely in-RAM, capacity-
// bounded (see lib/event_log). 40 entries is generous for "what happened
// recently" without denting RAM (~40 * a short sentence is a few KB).
// Exposed here, not just local to the .cpp, so webserver_activity.cpp's
// "most recent N events" hint text can't drift from the real capacity.
static const size_t EVENT_LOG_CAPACITY = 40;

// Appends one entry, timestamped with the current millis(). Cheap enough
// for a hot-ish path (motion alerts, on/off/online-offline transitions) -
// see event_log_store.cpp for what calls it and why.
void logEvent(const String& text);

// A copy of everything currently logged, oldest-first. Copied out, not a
// reference, so the caller (webserver_activity.cpp's render) doesn't hold
// the lock while building HTML.
std::vector<EventLogEntry> recentEvents();
