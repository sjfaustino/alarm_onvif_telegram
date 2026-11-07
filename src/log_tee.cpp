#include "log_tee.h"
#include "log_ring.h"
#include "config.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

LogTee g_logTee;

static SemaphoreHandle_t g_logMutex = xSemaphoreCreateMutex();
static LogRing* g_ring = nullptr;
static bool g_atLineStart = true;

void LogTee::begin(unsigned long baud) {
  logTeeRealSerial().begin(baud);
  // PSRAM only: never competes with mbedTLS for internal RAM. On a board without
  // it there's no ring, and setup() halts anyway.
  char* buffer = (char*)heap_caps_malloc(DEBUG_LOG_RING_BYTES, MALLOC_CAP_SPIRAM);
  if (buffer) g_ring = new LogRing(buffer, DEBUG_LOG_RING_BYTES);
}

size_t LogTee::write(uint8_t c) { return write(&c, 1); }

size_t LogTee::write(const uint8_t* data, size_t len) {
  size_t written = logTeeRealSerial().write(data, len);
  // Short bounded wait: a log line is never worth stalling a task over.
  if (!g_ring || xPortInIsrContext() || xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(20)) != pdTRUE) return written;
  // Stamp each line with uptime, since there's no terminal to timestamp it.
  const char* p = (const char*)data;
  const char* end = p + len;
  while (p < end) {
    if (g_atLineStart) {
      char stamp[16];
      int n = snprintf(stamp, sizeof(stamp), "[%lu] ", (unsigned long)millis());
      g_ring->write(stamp, (size_t)n);
      g_atLineStart = false;
    }
    const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
    const char* stop = nl ? nl + 1 : end;
    g_ring->write(p, (size_t)(stop - p));
    if (nl) g_atLineStart = true;
    p = stop;
  }
  xSemaphoreGive(g_logMutex);
  return written;
}

size_t logTeeRead(uint32_t from, char* out, size_t maxLen, uint32_t* next, uint32_t* oldest) {
  if (!g_ring || xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
  size_t n = g_ring->read(from, out, maxLen, next);
  if (oldest) *oldest = g_ring->oldest();
  xSemaphoreGive(g_logMutex);
  return n;
}

uint32_t logTeeEnd() {
  if (!g_ring || xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
  uint32_t end = g_ring->end();
  xSemaphoreGive(g_logMutex);
  return end;
}
