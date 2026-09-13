#include "telegram_retry_queue.h"
#include "telegram.h" // sendTelegramMessageToChatId
#include "config.h" // TELEGRAM_RETRY_QUEUE_CAPACITY/TELEGRAM_RETRY_MAX_AGE_MS
#include <vector>

struct QueuedTelegramMessage {
  String chatId;
  String text;
  unsigned long enqueuedAtMs;
};

// Same-task-only (main.cpp's loop() is the only caller of both
// enqueueFailedTelegramMessage - via sendTelegramMessage's failure branch,
// itself only ever called from loop()'s own task - and
// flushTelegramRetryQueue), no lock needed.
static std::vector<QueuedTelegramMessage> g_queue;

void enqueueFailedTelegramMessage(const String& chatId, const String& text) {
  if (g_queue.size() >= TELEGRAM_RETRY_QUEUE_CAPACITY) {
    g_queue.erase(g_queue.begin()); // drop the oldest to make room
  }
  g_queue.push_back({chatId, text, millis()});
}

void flushTelegramRetryQueue() {
  if (g_queue.empty()) return;

  unsigned long now = millis();
  std::vector<QueuedTelegramMessage> remaining;
  remaining.reserve(g_queue.size());
  for (auto& m : g_queue) {
    if (sendTelegramMessageToChatId(m.chatId, m.text)) continue; // delivered - drop
    // Subtraction, not comparison, tolerates millis() wraparound (~49
    // days uptime) the same way every other elapsed-time check in this
    // project does (e.g. net_watchdog_logic's outageThresholdReached).
    bool expired = (now - m.enqueuedAtMs) > TELEGRAM_RETRY_MAX_AGE_MS;
    if (expired) continue; // too old to still be useful - drop regardless of outcome
    remaining.push_back(m);
  }
  g_queue = remaining;
}
