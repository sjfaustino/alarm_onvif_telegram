#include "event_log_store.h"
#include "sd_store.h" // appendActivityLogLine - SD persistence of this same log, best-effort
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

static EventRingBuffer g_events(EVENT_LOG_CAPACITY);
static SemaphoreHandle_t g_eventsMutex = xSemaphoreCreateMutex();

// Own wall-clock formatter rather than depending on telegram.cpp's
// nowTimestampString() - small deliberate duplication instead of a new
// cross-file dependency for one line. millis() alone would be meaningless
// across a reboot in a file meant to persist past one.
static String wallClockTimestamp() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmStruct);
  return String(buf);
}

void logEvent(const String& text) {
  xSemaphoreTake(g_eventsMutex, portMAX_DELAY);
  g_events.push(millis(), text);
  xSemaphoreGive(g_eventsMutex);

  // After releasing the mutex above, not while held - appendActivityLogLine
  // can block on SD I/O (and, on failure, a Telegram send via
  // markSdFailed), which must never happen while holding this one.
  appendActivityLogLine(wallClockTimestamp() + " " + text);
}

std::vector<EventLogEntry> recentEvents() {
  xSemaphoreTake(g_eventsMutex, portMAX_DELAY);
  std::vector<EventLogEntry> copy = g_events.entries();
  xSemaphoreGive(g_eventsMutex);
  return copy;
}
