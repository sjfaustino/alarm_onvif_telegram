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

// Each check alerts once when its threshold is crossed and re-arms on
// recovery.
static bool g_nvsUsageAlerted = false;
static bool g_wifiRssiWeakAlerted = false;
static bool g_sdUsageAlerted = false;

// Heap state never re-arms: the minimum only falls within a boot.
static bool g_heapBaselineSet = false;
static uint32_t g_heapBaseline = 0;
static bool g_heapLowAlerted = false;

// One camera's state, read once under the lock before any per-language text is
// composed.
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
  // Min next to current reveals a leak: a falling minimum across heartbeats.
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minFreeHeap = ESP.getMinFreeHeap();
  // Shown every heartbeat so a slow climb is visible before the alert fires.
  nvs_stats_t nvsStats;
  bool haveNvsStats = nvs_get_stats(NULL, &nvsStats) == ESP_OK && nvsStats.total_entries > 0;
  unsigned nvsPct = haveNvsStats ? (unsigned)((uint64_t)nvsStats.used_entries * 100 / nvsStats.total_entries) : 0;
  int rssi = WiFi.RSSI();

  // Lock once here, not once per recipient language.
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
    // Show when a timed /on or /off reverts: "alerts OFF" alone looks
    // permanent.
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

// No-op without SD (a zero-size card would read as 100% full).
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

// Logs each new lifetime-low heap record with a timestamp so a leak or spike
// can be matched against other Activity entries. Cheap enough for every tick.
void checkHeapHealth() {
  HeapHealthResult r = evaluateHeapHealth(ESP.getMinFreeHeap(), g_heapBaselineSet, g_heapBaseline,
                                           HEAP_LOW_WARN_BYTES, g_heapLowAlerted);
  g_heapBaselineSet = true;
  g_heapBaseline = r.baseline;
  if (r.shouldAlert) g_heapLowAlerted = true;
  if (!r.shouldLog) return;

  // Largest free block: much smaller than total free means fragmentation, not
  // low memory.
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
