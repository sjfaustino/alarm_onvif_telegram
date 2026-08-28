#pragma once
#include <Arduino.h>
#include <vector>
#include "event_log.h" // EventLogEntry, EventRingBuffer - the pure ring buffer (lib/event_log)

// Thread-safe global event log - append from any task (camera tasks,
// loop()'s task) and read from any task (PsychicHttp's dashboard render).
// A thin FreeRTOS-mutex wrapper around EventRingBuffer (lib/event_log,
// unit-tested); this half isn't, same as every other bit of FreeRTOS glue
// in src/ - see test/README.md.
//
// Named event_log_store.h, not event_log.h, specifically so it can't
// collide with lib/event_log/event_log.h on the include path - two
// same-named headers reachable from the same translation unit would
// resolve to whichever one the compiler's search order happens to favor,
// silently and differently depending on flag order (this project already
// tracked down one real bug caused by exactly this kind of implicit,
// unverified ordering assumption - see platformio.ini's build_src_flags
// comment).

// Recent-activity view, not a permanent log - see this header's own
// comment and lib/event_log/event_log.h for why this is capacity-bounded
// and purely in-RAM. 40 entries is generous for "what happened in the
// last while" without meaningfully denting this board's RAM budget
// (String overhead aside, ~40 * (4 bytes + a short sentence) is a few KB
// at most). Exposed here, not just as a local in the .cpp, so
// webserver_activity.cpp's "most recent N events" hint text can't drift
// out of sync with the real capacity.
static const size_t EVENT_LOG_CAPACITY = 40;

// Appends one entry, timestamped with the current millis(). Cheap enough
// to call from any hot-ish path (motion alerts, on/off/online-offline
// transitions) - see event_log_store.cpp for what actually calls it and
// why those specific events were chosen.
void logEvent(const String& text);

// A copy of everything currently logged, oldest-first. Copied out (not a
// reference) so the caller (webserver_activity.cpp's render) doesn't hold
// the lock while building HTML.
std::vector<EventLogEntry> recentEvents();
