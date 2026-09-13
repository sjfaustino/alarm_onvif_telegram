#include "telegram_retry_queue.h"
#include "telegram.h" // sendTelegramMessageToChatId
#include "config.h" // TELEGRAM_RETRY_QUEUE_CAPACITY/TELEGRAM_RETRY_MAX_AGE_MS
#include <freertos/semphr.h> // SemaphoreHandle_t - explicit, not just via telegram.h's own chain
#include <vector>

struct QueuedTelegramMessage {
  // Monotonically increasing, assigned at enqueue - lets flush remove
  // exactly the entry it actually just sent by identity, safely, even if
  // the vector's contents shifted underneath it (another task's
  // concurrent enqueue evicting the front entry for capacity, or
  // appending a new one) while this one was unlocked mid-send. A plain
  // index or a copied snapshot's position would not survive that.
  uint32_t id;
  String chatId;
  String text;
  unsigned long enqueuedAtMs;
};

// g_queue is genuinely cross-task, NOT same-task-only (an earlier version
// of this file claimed otherwise - wrong): enqueueFailedTelegramMessage
// is reached via sendTelegramMessage's failure branch, and that function
// is called from EVERY camera's own task (checkCameraOnlineStatus/
// checkSubscriptionHealth/checkMotionWatchdog/checkMultiCameraAlertDigest,
// camera.cpp) as well as loop()'s own task (sendHeartbeat,
// checkScheduledAlertReverts, setupTime's NTP-failure alert, ...) - with
// more than one camera configured, that's several different tasks able
// to call it concurrently. flushTelegramRetryQueue() (loop()'s task) and
// getTelegramRetryQueueStatus() (any task - the dashboard render runs on
// PsychicHttp's own task) read/rewrite the same std::vector. Guarded here
// the same way CameraState::stateMutex guards its own cross-task fields.
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

// Removes the entry with this exact id, if it's still present - a no-op
// if another task's concurrent enqueue already evicted it for capacity
// in the meantime (rare, and harmless either way: that eviction already
// did this entry's job of removing it).
static void removeById(uint32_t id) {
  xSemaphoreTake(g_retryQueueMutex, portMAX_DELAY);
  for (size_t i = 0; i < g_queue.size(); i++) {
    if (g_queue[i].id == id) { g_queue.erase(g_queue.begin() + i); break; }
  }
  xSemaphoreGive(g_retryQueueMutex);
}

void flushTelegramRetryQueue() {
  // Copied out under the lock, sends attempted OUTSIDE it - same "never
  // hold a mutex across a blocking network call" discipline as every
  // other cross-task lock in this project (e.g. telegram.cpp's
  // checkMultiCameraAlertDigest). A message enqueued by another task
  // while sends are in flight here just waits for the next flush; each
  // send's own outcome is applied back via removeById (by stable id), not
  // by reassigning g_queue wholesale, so nothing concurrently added is
  // ever at risk of being silently dropped.
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
    // Subtraction, not comparison, tolerates millis() wraparound (~49
    // days uptime) the same way every other elapsed-time check in this
    // project does (e.g. net_watchdog_logic's outageThresholdReached).
    bool expired = !delivered && (now - m.enqueuedAtMs) > TELEGRAM_RETRY_MAX_AGE_MS;
    if (delivered || expired) removeById(m.id); // too old to still be useful - drop regardless of outcome
  }
}

TelegramRetryQueueStatus getTelegramRetryQueueStatus() {
  TelegramRetryQueueStatus status;
  xSemaphoreTake(g_retryQueueMutex, portMAX_DELAY);
  status.count = g_queue.size();
  // Oldest is always at index 0 - enqueue only ever removes from the
  // front (capacity eviction) or appends at the back, and removeById
  // closes a gap in place without reordering anything else, so insertion
  // order (oldest-first) is preserved no matter which entries get removed
  // out of order.
  if (status.count > 0) status.oldestPendingMs = millis() - g_queue[0].enqueuedAtMs;
  xSemaphoreGive(g_retryQueueMutex);
  return status;
}
