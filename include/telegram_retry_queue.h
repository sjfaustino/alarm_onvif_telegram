#pragma once
#include <Arduino.h>

// In-RAM retry queue for broadcast alerts that failed (usually WAN down). Not
// for command replies (confusing when late) or photos (too much memory to
// hold). Lost on reboot, like the Activity log.

// Oldest entry is dropped when full.
void enqueueFailedTelegramMessage(const String& chatId, const String& text);

// Call every TELEGRAM_RETRY_FLUSH_INTERVAL_MS while WiFi is up. Delivered
// entries are removed; entries older than TELEGRAM_RETRY_MAX_AGE_MS are
// dropped after this attempt either way.
void flushTelegramRetryQueue();

// Firmware page status. A growing oldestPendingMs means a sustained outage or
// a bad token/CA.
struct TelegramRetryQueueStatus {
  size_t count = 0;
  unsigned long oldestPendingMs = 0; // meaningful only if count > 0
};
TelegramRetryQueueStatus getTelegramRetryQueueStatus();
