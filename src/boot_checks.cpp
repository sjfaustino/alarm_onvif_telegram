#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_system.h>   // esp_reset_reason()
#include <esp_ota_ops.h>  // esp_ota_mark_app_valid_cancel_rollback()
#include "boot_checks.h"
#include "event_log_store.h"
#include "ota_history.h"
#include "telegram.h"

// Outlasts loop()'s longest blocking stretch (connectWiFi: 30s per network,
// which also feeds the watchdog).
static const uint32_t WATCHDOG_TIMEOUT_MS = 90000UL;

// Catches a frozen loop(), which the heartbeat can't. Camera tasks aren't
// watched: their SOAP calls are timeout-bounded, and back-to-back setup calls
// would give slow cameras false positives.
void initWatchdog() {
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // keep watching both cores' idle tasks too
    .trigger_panic = true,
  };
  // Newer cores pre-initialise the TWDT, so init fails with INVALID_STATE;
  // reconfigure instead.
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

// Reboot reason in English, for Serial, the Activity log and the boot notice.
// Kept out of lib/ because esp_reset_reason_t isn't available natively. Has a
// default so new IDF values read "unknown".
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

// Portuguese version for the boot notice.
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

// The boot notice just says expected vs. crash; the full reason goes to the
// Activity log.
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
  // Marks this image valid, cancelling OTA rollback; otherwise a working
  // update would still revert on the next reset. Runs at the end of setup(),
  // regardless of WiFi - getting here means init survived.
  //
  // wasPendingVerify is read before marking valid: only the first boot after
  // an update is PENDING_VERIFY, whereas the mark-valid call succeeds on every
  // boot.
  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
  bool wasPendingVerify = runningPartition &&
      esp_ota_get_state_partition(runningPartition, &otaState) == ESP_OK &&
      otaState == ESP_OTA_IMG_PENDING_VERIFY;

  esp_err_t rollbackErr = esp_ota_mark_app_valid_cancel_rollback();
  if (rollbackErr == ESP_OK) {
    Serial.println("OTA rollback: this firmware confirmed healthy - won't auto-revert on the next reboot.");
  }
  // Any other result is the normal no-rollback-pending case.

  // First boot after an update: log, alert and persist it. A separate message,
  // since the boot notice has already gone out.
  if (rollbackErr == ESP_OK && wasPendingVerify) {
    logEvent("OTA update confirmed healthy - rollback canceled");
    sendTelegramMessage([](TelegramLang lang) { return trOtaConfirmedHealthy(lang); });
    recordOtaOutcome(OtaOutcome::Healthy, runningPartition ? String(runningPartition->label) : "");
  }

  // An update the bootloader rolled back. The invalid partition stays reported
  // until another OTA overwrites that slot, so ota_history dedups it (and
  // forgets once the slot is valid again).
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
