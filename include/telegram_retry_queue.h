#pragma once
#include <Arduino.h>

// Bounded in-RAM (lost on reboot) retry queue for text-only, unsolicited
// Telegram alerts (systemMessages broadcasts - heartbeat, boot notice,
// watchdog outage/recovery, offline/online, subscription-health, motion-
// watchdog, scheduled-revert, NVS/heap warnings, ...) that failed to
// send, typically because WAN was down at the time. Deliberately NOT used
// for interactive command replies (stale and confusing if finally
// delivered minutes/hours later, out of the context they were sent in)
// or photo sends (retaining a JPEG buffer for later retry is a much
// bigger memory commitment this project isn't taking on for this pass).
// Purely in-RAM, same as the existing Activity log - these are inherently
// time-sensitive alerts, not something worth NVS wear/complexity to
// persist across a reboot.

// Records one message that failed to send, for a later retry once WAN
// connectivity is back. Oldest entry is dropped to make room once the
// queue is at TELEGRAM_RETRY_QUEUE_CAPACITY (config.h) - a long outage
// with several distinct alerts drops the earliest ones rather than
// growing unbounded; a dropped entry is simply never retried, not
// re-attempted forever.
void enqueueFailedTelegramMessage(const String& chatId, const String& text);

// Call periodically (main.cpp's loop(), on TELEGRAM_RETRY_FLUSH_INTERVAL_MS's
// own cadence, only while WiFi is connected - there's no point attempting
// otherwise). Attempts to resend everything currently queued, removing an
// entry once it's delivered. An entry older than TELEGRAM_RETRY_MAX_AGE_MS
// (config.h) is dropped after this attempt regardless of outcome - by
// then it's no longer useful information.
void flushTelegramRetryQueue();

// Status for the dashboard (Firmware page) - reflects live state, doesn't
// trigger a flush itself. An empty queue (the common, healthy case) is
// the "everything's being delivered fine" signal; a nonzero, growing
// oldestPendingMs alongside a nonzero count is what a sustained WAN
// outage (or a misconfigured bot token/CA cert, see telegram_ca.h) looks
// like from the dashboard - previously invisible entirely.
struct TelegramRetryQueueStatus {
  size_t count = 0;
  unsigned long oldestPendingMs = 0; // meaningful only if count > 0
};
TelegramRetryQueueStatus getTelegramRetryQueueStatus();
