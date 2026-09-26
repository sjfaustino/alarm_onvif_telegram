#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_system.h>   // esp_reset_reason()
#include <esp_ota_ops.h>  // esp_ota_mark_app_valid_cancel_rollback()
#include "boot_checks.h"
#include "event_log_store.h"
#include "ota_history.h"
#include "telegram.h"

// TWDT timeout for the Arduino loop() task - see initWatchdog(). Comfortably
// outlasts the longest stretch loop() can go without returning to its top
// (connectWiFi() trying primary then backup, 30s each); tryConnectWiFi also
// feeds the watchdog every 500ms while polling, so this is belt-and-suspenders.
static const uint32_t WATCHDOG_TIMEOUT_MS = 90000UL;

// Arms the TWDT against loop() so a genuinely frozen main loop reboots the
// board instead of hanging forever - the gap the Telegram heartbeat can't
// cover on its own. Deliberately not armed against the per-camera tasks:
// every SOAP call already has its own HTTP_TIMEOUT_MS bound, and
// cameraSetupSequence chains several back-to-back with no safe point to
// feed a per-task watchdog without risking a false positive on a slow camera.
void initWatchdog() {
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // keep watching both cores' idle tasks too
    .trigger_panic = true,
  };
  // Recent cores already init the TWDT by default (idle tasks only), which
  // makes esp_task_wdt_init() fail with ESP_ERR_INVALID_STATE - reconfigure
  // the running one instead of treating that as an error.
  esp_err_t err = esp_task_wdt_init(&wdtConfig);
  if (err == ESP_ERR_INVALID_STATE) {
    err = esp_task_wdt_reconfigure(&wdtConfig);
  }
  if (err != ESP_OK) {
    Serial.printf("WARNING: task watchdog init failed (err=%d) - a hung loop() won't self-recover.\n",
                  (int)err);
    return;
  }

  esp_task_wdt_add(nullptr); // nullptr = subscribe the calling task (loopTask, since setup() runs on it too)
  Serial.printf("Task watchdog armed on loop(): %lus timeout, reboots the board if it hangs.\n",
                (unsigned long)(WATCHDOG_TIMEOUT_MS / 1000UL));
}

// Human text for esp_reset_reason() - folded into the boot Telegram message
// and an early Serial line, so "why did it reboot" doesn't need Serial
// watched at the exact moment. Not a lib/ pure function like this
// project's other testable logic: esp_reset_reason_t is an ESP-IDF type
// unavailable under the native test environment, and this is a plain
// enum->string table. Deliberately has a `default:` (unlike this
// project's own TelegramCommand switches) - esp_reset_reason_t belongs to
// the framework, so a future IDF enumerator should fall through to
// "unknown", not force a rebuild-breaking change here.
String describeResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SW:        return "software (ESP.restart() - /reset command, a firmware update, or a "
                                    "Maintenance page reboot)";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog (a task hung - see initWatchdog())";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake (unexpected - this project never sleeps)";
    case ESP_RST_BROWNOUT:  return "brownout (power dip/insufficient supply)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
  }
}

// European Portuguese (pt-PT) counterpart of describeResetReason() above,
// used only for the boot Telegram message (Serial always stays English -
// see that function's own comment on why this table isn't in
// lib/telegram_i18n).
static String describeResetReasonPt() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "liga\xC3\xA7\xC3\xA3o";
    case ESP_RST_EXT:       return "pino de reset externo";
    case ESP_RST_SW:        return "software (comando /reset, uma atualiza\xC3\xA7\xC3\xA3o de firmware, ou um "
                                    "rein\xC3\xAD" "cio a partir da p\xC3\xA1gina Manuten\xC3\xA7\xC3\xA3o)";
    case ESP_RST_PANIC:     return "PANIC (falha)";
    case ESP_RST_INT_WDT:   return "watchdog de interrup\xC3\xA7\xC3\xA3o";
    case ESP_RST_TASK_WDT:  return "watchdog de tarefa (uma tarefa travou - veja initWatchdog())";
    case ESP_RST_WDT:       return "outro watchdog";
    case ESP_RST_DEEPSLEEP: return "despertar de deep sleep (inesperado - este projeto nunca dorme)";
    case ESP_RST_BROWNOUT:  return "brownout (queda de energia/alimenta\xC3\xA7\xC3\xA3o insuficiente)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "desconhecido";
  }
}

static String describeResetReasonLocalized(TelegramLang lang) {
  return lang == TelegramLang::Portuguese ? describeResetReasonPt() : describeResetReason();
}

// Simplified two-way split of describeResetReason() above, for the
// Telegram boot notice specifically (trRebootReasonLine) - the full
// detail (which watchdog, brownout, etc.) stays in the Activity log via
// logEvent("Booted: " + describeResetReason()) below; someone reading a
// phone notification just needs "expected" vs "something crashed," not
// the full esp_reset_reason_t taxonomy.
static bool isCrashResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
    case ESP_RST_EXT:
    case ESP_RST_SW:
      return false; // power-on, external reset pin, or a deliberate /reset|OTA|Maintenance-page reboot
    default:
      return true; // PANIC, every watchdog flavor, brownout, unexpected deep sleep/SDIO, unknown
  }
}

String describeResetReasonShort(TelegramLang lang) {
  bool pt = lang == TelegramLang::Portuguese;
  if (isCrashResetReason()) {
    return pt ? "\xE2\x9A\xA0\xEF\xB8\x8F falha inesperada" : "\xE2\x9A\xA0\xEF\xB8\x8F unexpected crash";
  }
  return pt ? "normal (utilizador/liga\xC3\xA7\xC3\xA3o)" : "normal (user/power-on)";
}

void confirmFirmwareAndReportRollback() {
  // Confirms this firmware image is healthy, canceling ESP-IDF's OTA
  // rollback safety net (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is set).
  // Without this, firmware flashed via /firmware/update that boot-loops
  // gets auto-reverted to the previous working partition on the next
  // reset - otherwise a bad OTA upload would permanently strand the board
  // until someone gets a USB cable to it.
  //
  // Placed at the end of setup(), not gated on WiFi connecting above - a
  // network outage during an update shouldn't roll back otherwise-good
  // firmware, and reaching this line already means every crash-prone init
  // step survived without a panic or watchdog reset.
  //
  // wasPendingVerify is captured BEFORE the mark-valid call below, which
  // is exactly what changes it - PENDING_VERIFY is the state ONLY the
  // very first boot after a fresh OTA flash starts in; every ordinary
  // boot after that already reads VALID. esp_ota_mark_app_valid_cancel_rollback()
  // returning ESP_OK on its own can't tell those apart - it succeeds on
  // literally every boot, not just a just-flashed one - so logging/
  // alerting on ESP_OK alone would fire forever, not just once per update.
  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
  bool wasPendingVerify = runningPartition &&
      esp_ota_get_state_partition(runningPartition, &otaState) == ESP_OK &&
      otaState == ESP_OTA_IMG_PENDING_VERIFY;

  esp_err_t rollbackErr = esp_ota_mark_app_valid_cancel_rollback();
  if (rollbackErr == ESP_OK) {
    Serial.println("OTA rollback: this firmware confirmed healthy - won't auto-revert on the next reboot.");
  }
  // Any other result (e.g. no rollback pending - the normal case except
  // right after a firmware update) is expected, not logged as an error.

  // Only on the one boot that actually just cleared a pending OTA
  // validation (see wasPendingVerify above) - a durable Activity-log line
  // plus a Telegram push, since previously the only confirmation this
  // ever happened was the Serial.println above, invisible unless someone
  // had a cable plugged in at that exact reboot. Sent as its own message,
  // not folded into startMonitoring()'s earlier boot notice - this call
  // is deliberately AFTER that one runs (see this whole block's own
  // comment on why), so that message has already gone out by the time
  // this is known.
  if (rollbackErr == ESP_OK && wasPendingVerify) {
    logEvent("OTA update confirmed healthy - rollback canceled");
    sendTelegramMessage([](TelegramLang lang) { return trOtaConfirmedHealthy(lang); });
    // Persisted (ota_history.h) so the Firmware page can show this
    // outcome from then on, not just the one-time push above - otherwise
    // the only record of it ever having happened is whatever's left in
    // the Activity log's own bounded/rotating history.
    recordOtaOutcome(OtaOutcome::Healthy, runningPartition ? String(runningPartition->label) : "");
  }

  // The counterpart the block above never had: a firmware update that
  // boot-looped and got auto-reverted by the bootloader's own rollback
  // safety net was previously invisible - the board would just quietly
  // come back up on the old, previously-good partition with no record
  // anything had gone wrong. esp_ota_get_last_invalid_partition() is
  // ESP-IDF's own record of which partition (if any) most recently failed
  // validation - unlike wasPendingVerify above, this stays set across
  // every later boot until that same slot gets overwritten by another OTA
  // attempt, so alerting on it unconditionally would repeat forever;
  // ota_history.h's dedup marker remembers which failure this board
  // already reported, and is cleared again once the slot stops being
  // invalid (a fresh, successful OTA overwrote it), so a FUTURE rollback
  // onto that same slot is still caught fresh.
  const esp_partition_t* invalidPartition = esp_ota_get_last_invalid_partition();
  String invalidLabel = invalidPartition ? String(invalidPartition->label) : "";
  if (invalidLabel.length() > 0 && !alreadyReportedInvalidPartition(invalidLabel)) {
    logEvent("OTA update failed validation - rolled back to the previous firmware (" + invalidLabel +
             " marked invalid)");
    sendTelegramMessage([invalidLabel](TelegramLang lang) { return trOtaRolledBack(lang, invalidLabel); });
    rememberReportedInvalidPartition(invalidLabel);
    recordOtaOutcome(OtaOutcome::RolledBack, invalidLabel); // same "persist for the Firmware page" reasoning as the healthy case above
  } else if (invalidLabel.length() == 0) {
    forgetReportedInvalidPartition();
  }
}
