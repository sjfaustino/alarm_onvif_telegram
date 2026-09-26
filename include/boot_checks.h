#pragma once
#include <Arduino.h>
#include "telegram_i18n.h" // TelegramLang

// Arms the task watchdog on the calling task (loop()'s).
void initWatchdog();

// esp_reset_reason() as English text, for Serial and the Activity log.
String describeResetReason();
// "normal" vs "unexpected crash", localized, for the Telegram boot notice.
String describeResetReasonShort(TelegramLang lang);

// Marks the running image valid (cancels OTA rollback) and reports, once
// each, a just-confirmed OTA update or a rollback the bootloader performed.
// Call at the end of setup().
void confirmFirmwareAndReportRollback();
