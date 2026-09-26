#include "telegram_retry_queue.h"
#include "telegram.h" // sendTelegramMessageToChatId
#include "config.h" // TELEGRAM_RETRY_QUEUE_CAPACITY/TELEGRAM_RETRY_MAX_AGE_MS
#include <freertos/semphr.h> // SemaphoreHandle_t - explicit, not just via telegram.h's own chain
#include <vector>

struct QueuedTelegramMessage {
  // Stable id, so flush removes exactly what it sent even if other tasks
  // changed the vector while it was unlocked.
  uint32_t id;
  String chatId;
  String text;
  unsigned long enqueuedAtMs;
};

// Cross-task: enqueue happens from every camera task and loop() (via failed
// broadcasts), flush from loop(), status from the web server.
static SemaphoreHandle_t g_retryQueueMutex = xSemaphoreCreateMutex();
static std::vector<QueuedTelegramMessage> g_queue;
static uint32_t g_nextId = 1;

void enqueueFailedTelegramMessage(const String& chatId, const String& text) {
  xSemaphoreTake(g_retryQueueMutex, portMAX_DELAY);
  if (g_queue.size() >= TELEGRAM_RETRY_QUEUE_CAPACITY) {
    g_queue.erase(g_queue.begin()); // drop the oldest to make room
  }
  g_queue.push_back({g_nextId++, chatId, text, millis()});
  xSemaphoreGive(g_retryQueueMutex);
}

// No-op if already evicted for capacity.
static void removeById(uint32_t id) {
  xSemaphoreTake(g_retryQueueMutex, portMAX_DELAY);
  for (size_t i = 0; i < g_queue.size(); i++) {
    if (g_queue[i].id == id) { g_queue.erase(g_queue.begin() + i); break; }
  }
  xSemaphoreGive(g_retryQueueMutex);
}

void flushTelegramRetryQueue() {
  // Copy under the lock, send outside it. Results are applied by id, so
  // messages enqueued meanwhile are never dropped.
  std::vector<QueuedTelegramMessage> snapshot;
  {
    xSemaphoreTake(g_retryQueueMutex, portMAX_DELAY);
    snapshot = g_queue;
    xSemaphoreGive(g_retryQueueMutex);
  }
  if (snapshot.empty()) return;

  unsigned long now = millis();
  for (auto& m : snapshot) {
    bool delivered = sendTelegramMessageToChatId(m.chatId, m.text);
    // Subtraction survives millis() wraparound.
    bool expired = !delivered && (now - m.enqueuedAtMs) > TELEGRAM_RETRY_MAX_AGE_MS;
    if (delivered || expired) removeById(m.id); // too old to still be useful - drop regardless of outcome
  }
}

TelegramRetryQueueStatus getTelegramRetryQueueStatus() {
  TelegramRetryQueueStatus status;
  xSemaphoreTake(g_retryQueueMutex, portMAX_DELAY);
  status.count = g_queue.size();
  // Insertion order is preserved, so index 0 is the oldest.
  if (status.count > 0) status.oldestPendingMs = millis() - g_queue[0].enqueuedAtMs;
  xSemaphoreGive(g_retryQueueMutex);
  return status;
}
