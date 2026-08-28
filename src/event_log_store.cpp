#include "event_log_store.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static EventRingBuffer g_events(EVENT_LOG_CAPACITY);
static SemaphoreHandle_t g_eventsMutex = xSemaphoreCreateMutex();

void logEvent(const String& text) {
  xSemaphoreTake(g_eventsMutex, portMAX_DELAY);
  g_events.push(millis(), text);
  xSemaphoreGive(g_eventsMutex);
}

std::vector<EventLogEntry> recentEvents() {
  xSemaphoreTake(g_eventsMutex, portMAX_DELAY);
  std::vector<EventLogEntry> copy = g_events.entries();
  xSemaphoreGive(g_eventsMutex);
  return copy;
}
