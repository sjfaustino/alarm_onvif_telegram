#include <Arduino.h>
#include <WiFi.h>
#include <nvs_flash.h>    // nvs_get_stats()
#include <nvs.h>
#include "health_monitor.h"
#include "camera_tasks.h" // g_cameras/g_cameraStates
#include "build_version.h"
#include "config.h"
#include "event_log_store.h"
#include "heap_health.h"
#include "sd_store.h"
#include "telegram.h"
#include "telegram_i18n.h"

// True once checkNvsUsage() has already alerted for the current high-usage
// stretch - re-armed (set back false) once usage drops back under
// NVS_USAGE_WARN_PERCENT, same "alert once per state transition, not every
// check" pattern as CameraState::isOffline (telegram_alerts.cpp's
// checkCameraOnlineStatus).
static bool g_nvsUsageAlerted = false;
// Same alert-once-per-transition/re-arm shape as g_nvsUsageAlerted above,
// for checkWifiSignal() instead of checkNvsUsage().
static bool g_wifiRssiWeakAlerted = false;
// Same alert-once-per-transition/re-arm shape as g_nvsUsageAlerted above,
// for checkSdUsage() instead of checkNvsUsage() - see
// SD_USAGE_WARN_PERCENT's own comment (config.h) for why this exists.
static bool g_sdUsageAlerted = false;

// checkHeapHealth()'s own state - see evaluateHeapHealth's own comment
// (lib/heap_health) for why this never re-arms (a lifetime-low watermark
// only ever decreases within a boot, unlike NVS usage/WiFi RSSI above).
static bool g_heapBaselineSet = false;
static uint32_t g_heapBaseline = 0;
static bool g_heapLowAlerted = false;

// Periodic "still alive" ping - a missing heartbeat (or an unexpected boot
// message between expected ones) is the signal something's wrong.
// One camera's already-gathered raw state, snapshotted once (under lock)
// before composing any text - see sendHeartbeat's own comment for why.
struct HeartbeatCameraSnapshot {
  String name;
  bool subscribed = false;
  bool offline = false;
  bool alertsEnabled = false;
  bool revertPending = false;
  bool revertToOn = false;
  String untilTime;
};

void sendHeartbeat() {
  // The lifetime minimum next to the current value is what reveals a slow
  // leak: dropping every heartbeat means something's leaking; flat means
  // it's just normal steady-state overhead (WiFi/mbedTLS/PsychicHttp/tasks).
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minFreeHeap = ESP.getMinFreeHeap();
  // Same nvs_get_stats call webserver_firmware.cpp's Firmware page and
  // checkNvsUsage() below both use - see NVS_USAGE_WARN_PERCENT's own
  // comment (config.h) for why this is worth watching at all. Folded into
  // every heartbeat too, not just the proactive alert below, so a slow
  // climb toward the threshold is visible before it's actually crossed.
  nvs_stats_t nvsStats;
  bool haveNvsStats = nvs_get_stats(NULL, &nvsStats) == ESP_OK && nvsStats.total_entries > 0;
  unsigned nvsPct = haveNvsStats ? (unsigned)((uint64_t)nvsStats.used_entries * 100 / nvsStats.total_entries) : 0;
  int rssi = WiFi.RSSI();

  // subscriptionActive/isOffline are written by each camera's own task;
  // this runs on loop()'s task, so reading them needs CameraStateLock -
  // see CameraState::stateMutex. Gathered ONCE here, before any text is
  // composed, rather than inside the per-recipient-language composer below
  // - the lock/read doesn't need to happen once per recipient, only the
  // (much cheaper) text formatting does.
  std::vector<HeartbeatCameraSnapshot> cams;
  for (size_t i = 0; i < g_cameras.size(); i++) {
    if (!g_cameras[i].enabled) continue;
    HeartbeatCameraSnapshot snap;
    snap.name = g_cameras[i].name;
    unsigned long revertDueMs;
    {
      CameraStateLock lock(g_cameraStates[i]);
      snap.subscribed = g_cameraStates[i].subscriptionActive;
      snap.offline = g_cameraStates[i].isOffline;
      snap.alertsEnabled = g_cameraStates[i].alertsEnabled;
      revertDueMs = g_cameraStates[i].scheduledRevertDueMs;
      snap.revertToOn = g_cameraStates[i].scheduledRevertToOn;
    }
    // A pending timed /on or /off (see checkScheduledAlertReverts) says
    // WHEN it'll flip back, not just that it will - "alerts OFF" alone
    // looks identical whether that's permanent or about to auto-resume in
    // a minute, which is exactly the ambiguity worth resolving in a
    // heartbeat someone might only glance at.
    snap.revertPending = revertDueMs != 0 && (long)(millis() - revertDueMs) < 0;
    snap.untilTime = snap.revertPending ? formatLocalClockTime(revertDueMs) : "";
    cams.push_back(snap);
  }

  unsigned long uptimeMs = millis();
  bool sendOk = sendTelegramMessage([=](TelegramLang lang) {
    String msg = trHeartbeatHeader(lang, FIRMWARE_VERSION) + "\n";
    msg += trUptimeLine(lang, uptimeMs) + "\n";
    msg += trFreeHeapLine(lang, freeHeap, minFreeHeap) + "\n";
    if (haveNvsStats) msg += trNvsUsageLine(lang, nvsPct) + "\n";
    msg += trWifiSignalLine(lang, rssi) + "\n";
    for (auto& snap : cams) {
      msg += trHeartbeatCameraLine(lang, snap.name, snap.subscribed, snap.offline, snap.alertsEnabled,
                                    snap.revertPending, snap.revertToOn, snap.untilTime) + "\n";
    }
    return msg;
  });
  if (!sendOk) {
    Serial.println("Heartbeat: Telegram send failed.");
  }
}

// Proactive counterpart to the Firmware page's own NVS-usage hint
// (webserver_firmware.cpp) - see NVS_USAGE_WARN_PERCENT's comment
// (config.h) for why this is worth alerting on at all. Alerts once on
// crossing the threshold, not every call - see g_nvsUsageAlerted's own
// comment for the re-arm rule.
void checkNvsUsage() {
  nvs_stats_t nvsStats;
  if (nvs_get_stats(NULL, &nvsStats) != ESP_OK || nvsStats.total_entries == 0) return;

  unsigned pct = (unsigned)((uint64_t)nvsStats.used_entries * 100 / nvsStats.total_entries);
  bool highNow = pct >= NVS_USAGE_WARN_PERCENT;
  if (highNow == g_nvsUsageAlerted) return; // no state change since the last check

  g_nvsUsageAlerted = highNow;
  if (highNow) {
    Serial.printf("WARNING: NVS usage at %u%% (%u/%u entries) - this project has silently dropped "
                  "writes here before once it filled up.\n",
                  pct, (unsigned)nvsStats.used_entries, (unsigned)nvsStats.total_entries);
    logEvent("NVS usage at " + String(pct) + "% - approaching the point writes can start silently failing");
    sendTelegramMessage([pct](TelegramLang lang) { return trNvsUsageWarning(lang, pct); });
  } else {
    Serial.printf("NVS usage back under %u%% (%u%%).\n", NVS_USAGE_WARN_PERCENT, pct);
    logEvent("NVS usage back under " + String((unsigned)NVS_USAGE_WARN_PERCENT) + "% (" + String(pct) + "%)");
  }
}

// Same shape as checkNvsUsage above (alert once on crossing the
// threshold, re-arm on recovery) - see SD_USAGE_WARN_PERCENT's own
// comment (config.h) for why this is worth watching proactively, ahead of
// sd_store.cpp's own reactive trSdFailure alert. No-op if SD isn't active
// at all - getSdStatus() would just report totalBytes/usedBytes as 0,
// which would misreport as "100% full" instead of "not applicable".
void checkSdUsage() {
  if (!sdActive()) return;

  SdStatus status = getSdStatus();
  if (status.totalBytes == 0) return; // shouldn't happen while active, but never divide by zero

  unsigned pct = (unsigned)((status.usedBytes * 100) / status.totalBytes);
  bool highNow = pct >= SD_USAGE_WARN_PERCENT;
  if (highNow == g_sdUsageAlerted) return; // no state change since the last check

  g_sdUsageAlerted = highNow;
  if (highNow) {
    Serial.printf("WARNING: SD card usage at %u%% - check the Storage page's retention setting.\n", pct);
    logEvent("SD card usage at " + String(pct) + "% - approaching full");
    sendTelegramMessage([pct](TelegramLang lang) { return trSdUsageWarning(lang, pct); });
  } else {
    Serial.printf("SD card usage back under %u%% (%u%%).\n", SD_USAGE_WARN_PERCENT, pct);
    logEvent("SD card usage back under " + String((unsigned)SD_USAGE_WARN_PERCENT) + "% (" + String(pct) + "%)");
  }
}

// Same shape as checkNvsUsage above (alert once on crossing the threshold,
// re-arm on recovery) - see WIFI_RSSI_WARN_DBM's own comment (config.h)
// for why this is worth watching. Only meaningful while actually
// connected - called from loop() gated on WiFi.status()==WL_CONNECTED,
// same as every other WiFi-dependent periodic check there.
void checkWifiSignal() {
  int32_t rssi = WiFi.RSSI();
  bool weakNow = rssi <= WIFI_RSSI_WARN_DBM;
  if (weakNow == g_wifiRssiWeakAlerted) return; // no state change since the last check

  g_wifiRssiWeakAlerted = weakNow;
  if (weakNow) {
    Serial.printf("WARNING: WiFi signal weak (%d dBm, warn threshold %d dBm).\n", (int)rssi, WIFI_RSSI_WARN_DBM);
    logEvent("WiFi signal weak (" + String(rssi) + " dBm)");
    sendTelegramMessage([rssi](TelegramLang lang) { return trWifiWeakWarning(lang, (int)rssi); });
  } else {
    Serial.printf("WiFi signal back above %d dBm (%d dBm).\n", WIFI_RSSI_WARN_DBM, (int)rssi);
    logEvent("WiFi signal back above " + String(WIFI_RSSI_WARN_DBM) + " dBm (" + String(rssi) + " dBm)");
  }
}

// /health and the heartbeat already show ESP.getMinFreeHeap() as a bare
// number with no context - this gives it a timestamped trail instead, so a
// slow leak or a sudden burst (a multi-camera TLS/allocation spike) can
// actually be correlated against what else was happening in the Activity
// log around the same time. See evaluateHeapHealth's own comment
// (lib/heap_health) for the pure decision logic this wraps; called every
// loop() tick, not gated behind an interval - see HEAP_LOW_WARN_BYTES'
// own comment (config.h) for why that's cheap enough to do unconditionally.
void checkHeapHealth() {
  HeapHealthResult r = evaluateHeapHealth(ESP.getMinFreeHeap(), g_heapBaselineSet, g_heapBaseline,
                                           HEAP_LOW_WARN_BYTES, g_heapLowAlerted);
  g_heapBaselineSet = true;
  g_heapBaseline = r.baseline;
  if (r.shouldAlert) g_heapLowAlerted = true;
  if (!r.shouldLog) return;

  // Largest single allocatable block, not just the free-byte total - same
  // stat sendTelegramPhotoBuffered (telegram_transport.cpp) already logs before every
  // TLS send, for the same reason: a free-heap number much bigger than this
  // one means a FRAGMENTED heap (plenty of free bytes, none of them
  // contiguous enough for whatever allocation actually failed), a different
  // problem to chase than genuinely low total memory.
  uint32_t maxAlloc = ESP.getMaxAllocHeap();

  if (r.isNewLow) {
    Serial.printf("New lifetime-low free heap: %u bytes (largest block: %u).\n",
                  (unsigned)r.baseline, (unsigned)maxAlloc);
    logEvent("New low free heap record: " + String(r.baseline) + " bytes (largest block: " +
             String(maxAlloc) + ")");
  } else {
    Serial.printf("Free heap minimum so far this boot: %u bytes (largest block: %u).\n",
                  (unsigned)r.baseline, (unsigned)maxAlloc);
    logEvent("Free heap minimum so far this boot: " + String(r.baseline) + " bytes (largest block: " +
             String(maxAlloc) + ")");
  }
  if (r.shouldAlert) {
    uint32_t baseline = r.baseline;
    sendTelegramMessage([baseline, maxAlloc](TelegramLang lang) {
      return trHeapLowWarning(lang, baseline, maxAlloc);
    });
  }
}
