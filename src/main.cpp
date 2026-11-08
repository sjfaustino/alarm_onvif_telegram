#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "build_version.h"
#include "camera.h"
#include "camera_store.h"
#include "network_store.h"
#include "telegram.h"
#include "telegram_users.h"
#include "webserver.h"
#include "format_utils.h"
#include "event_log_store.h"
#include "camera_tasks.h"
#include "sd_store.h"
#include "rtc_store.h"
#include "net_watchdog.h"
#include "bridge_watchdog.h"
#include "power_monitor.h"
#include "telegram_retry_queue.h"
#include "telegram_i18n.h"
#include "wifi_connect.h"
#include "time_sync.h"
#include "boot_checks.h"
#include "health_monitor.h"
#include "diagnostics.h"

static unsigned long lastHeartbeatMs = 0;
static unsigned long lastCommandPollMs = 0;
static unsigned long lastSdCheckMs = 0;
static unsigned long lastRetentionCheckMs = 0;
static unsigned long lastNvsCheckMs = 0;
static unsigned long lastDigestMs = 0;
static unsigned long lastWifiRssiCheckMs = 0;
static unsigned long lastSdUsageCheckMs = 0;
static unsigned long lastNetWatchdogCheckMs = 0;
static unsigned long lastBridgeWatchdogCheckMs = 0;
static unsigned long lastPowerMonitorCheckMs = 0;
static unsigned long lastCameraTaskCheckMs = 0;
static unsigned long lastTelegramRetryFlushMs = 0;

// Set once startMonitoring() has run (may be later than setup()).
static bool g_monitoringStarted = false;

static void printCameraList() {
  Serial.println("\n--- Configured cameras ---");
  for (size_t i = 0; i < g_cameras.size(); i++) {
    const CameraConfig& cfg = g_cameras[i];
    Serial.printf("  [%u] %-20s %-24s %s\n",
                  (unsigned)(i + 1), cfg.name.c_str(), extractHost(cfg.deviceServiceUrl).c_str(),
                  cfg.enabled ? "enabled" : "disabled");
  }
  Serial.println("--------------------------\n");
}

static String buildCameraListMessage(TelegramLang lang) {
  bool pt = lang == TelegramLang::Portuguese;
  String s = trConfiguredCamerasHeader(lang) + "\n";
  for (size_t i = 0; i < g_cameras.size(); i++) {
    const CameraConfig& cfg = g_cameras[i];
    char line[64];
    // Checkmark for enabled; OFF stays text since that's the one worth
    // reading.
    if (cfg.enabled) {
      snprintf(line, sizeof(line), "  [%u] %-20s \xE2\x9C\x85\n", (unsigned)(i + 1), cfg.name.c_str());
    } else {
      snprintf(line, sizeof(line), "  [%u] %-20s %s\n", (unsigned)(i + 1), cfg.name.c_str(),
               pt ? "DESLIGADA" : "OFF");
    }
    s += line;
  }
  s += "--------------------------";
  return s;
}

// Everything that needs the network: NTP, mDNS, the dashboard, camera tasks.
// Runs after the first successful connect, so an outage at boot only delays
// monitoring.
static void startMonitoring() {
  setupTime();

  // Re-sanitized here because an imported config bypasses the form.
  String safeHostname = sanitizeHostname(g_wifiCredentials.hostname);
  if (MDNS.begin(safeHostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: reachable at http://%s.local/\n", safeHostname.c_str());
  } else {
    Serial.println("WARNING: mDNS.begin() failed - the .local hostname won't resolve; "
                    "the IP address still works.");
  }

  startWebServer(&g_cameras, &g_cameraStates); // logs its own "listening on http://<ip>:80/" line

  // Staggered: some cameras accept only 1-2 connections, and starting them all
  // at once caused connection resets at boot.
  for (size_t i = 0; i < g_cameras.size(); i++) {
    if (!g_cameras[i].enabled) {
      Serial.printf("[%s] Disabled - no task created.\n", g_cameras[i].name.c_str());
      continue;
    }
    spawnCameraTask(i);
    delay(750); // stagger initial GetCapabilities calls - see comment above
  }

  int enabledCount = 0;
  for (size_t i = 0; i < g_cameras.size(); i++) if (g_cameras[i].enabled) enabledCount++;

  // Logged here rather than earlier: the in-RAM log is wiped by the reboot, so
  // this is the first entry that survives into the new session.
  logEvent("Booted: " + describeResetReason());

  QuickSnapshotCheckResult sdBootCheck = lastBootCheckResult();
  bool sdBootCheckFailed = sdBootCheck.ranAtAll && !sdBootCheck.ok;
  size_t sdBootUnreadable = sdBootCheck.unreadableFiles;
  size_t sdBootDirsChecked = sdBootCheck.directoriesChecked;
  bool powerMonitorEnabled = powerMonitorActive();
  bool powerPresentAtBoot = getPowerMonitorStatus().powerPresent;
  // SD enabled but never mounted - sd_store couldn't alert before WiFi.
  SdStatus sdStatus = getSdStatus();
  bool sdUnavailableAtBoot = sdStatus.settingEnabled && !sdStatus.available;
  bool sendOk = sendTelegramMessage([enabledCount, sdBootCheckFailed, sdBootUnreadable, sdBootDirsChecked,
                                      powerMonitorEnabled, powerPresentAtBoot,
                                      sdUnavailableAtBoot](TelegramLang lang) {
    String msg = trBootHeader(lang, FIRMWARE_VERSION) + "\n";
    msg += trRebootReasonLine(lang, describeResetReasonShort(lang)) + "\n";
    msg += trEnabledCamerasLine(lang, (size_t)enabledCount, g_cameras.size()) + "\n";
    if (powerMonitorEnabled) msg += trPowerStatusLine(lang, powerPresentAtBoot) + "\n";
    if (sdUnavailableAtBoot) msg += trSdNotAvailableAtBoot(lang) + "\n";
    msg += buildCameraListMessage(lang);
    if (sdBootCheckFailed) {
      msg += "\n" + trSdBootCheckWarning(lang, sdBootUnreadable, sdBootDirsChecked);
    }
    return msg;
  });
  if (!sendOk) {
    Serial.println("Boot notice: Telegram send failed (network/token/CA issue?) - continuing anyway.");
  }

  lastHeartbeatMs = millis(); // first heartbeat fires HEARTBEAT_INTERVAL_MS from now, not immediately
  lastDigestMs = millis(); // first daily activity digest fires DAILY_DIGEST_INTERVAL_MS from now, not immediately
  // Boot already checked the newest files, so the first full check waits a
  // whole interval.
  lastSdCheckMs = millis();
  lastRetentionCheckMs = millis(); // first sweep fires SD_RETENTION_CHECK_INTERVAL_MS from now
  g_monitoringStarted = true;
}

void setup() {
  Serial.begin(115200);
  delay(BOOT_SETTLE_MS);
  initDiagnostics();
  Serial.println("\n========================================");
  Serial.println("MULTI-CAMERA ONVIF MOTION MONITOR");
  Serial.println("========================================");
  Serial.printf("Reboot reason: %s\n", describeResetReason().c_str());

  Serial.printf("PSRAM: %u bytes%s\n", (unsigned)ESP.getPsramSize(),
                ESP.getPsramSize() == 0 ? " (none detected)" : "");

  // PSRAM is mandatory: alert photos are buffered once and resent per
  // recipient, and without PSRAM that competes with TLS buffers and fails
  // mid-send. Refuse to start instead.
  if (ESP.getPsramSize() == 0) {
    Serial.println("\n========================================");
    Serial.println("FATAL: this board has no PSRAM.");
    Serial.println("This project requires a PSRAM-equipped board (e.g. ESP32-S3 with");
    Serial.println("embedded octal PSRAM - see platformio.ini's [env:esp32s3]).");
    Serial.println("Refusing to start rather than run degraded and fail later.");
    Serial.println("========================================");
    while (true) {
      delay(5000);
      Serial.println("HALTED: no PSRAM detected - see message above. Reflash onto a PSRAM board.");
    }
  }

  // Large allocations (>=4KB, e.g. SOAP responses) go to PSRAM, away from
  // mbedTLS; small ones stay internal, where access is faster.
  heap_caps_malloc_extmem_enable(4096);
  Serial.println("PSRAM: allocations >=4KB will be routed here automatically.");

  // After the PSRAM gate: that halt is deliberate, not something to reboot out
  // of.
  initWatchdog();

  // Before camera tasks start, so sdActive() is settled.
  initSdStorage();

  // Before WiFi, so the RTC can seed the clock first.
  initRtc();
  seedSystemClockFromRtc();

  // Early, so the relay never stays in a "pulsed" state across a reboot.
  initNetWatchdog();

  // Init order matters: each checks its pin against the ones loaded before it.
  initBridgeWatchdog();

  initPowerMonitor();

  g_wifiCredentials = loadWifiCredentials();

  // Self-gating one-time recovery; safe to leave in.

  // Loads the Telegram users into RAM now, while only this task runs: camera
  // tasks (PSRAM stacks) must never be the first to read them from NVS.
  loadTelegramUsers();

  // Reserve MAX_CAMERAS before any task exists: tasks hold raw pointers into
  // these vectors, and the headroom lets a live add push_back without
  // reallocating.
  g_cameras = loadCameras();
  g_cameraStates.resize(g_cameras.size());
  g_cameras.reserve(MAX_CAMERAS);
  g_cameraStates.reserve(MAX_CAMERAS);
  Serial.printf("Cameras configured: %u\n", (unsigned)g_cameras.size());

  printCameraList();

  if (!telegramCAConfigured()) {
    Serial.println("WARNING: telegram_ca.h's TELEGRAM_ROOT_CA is still the placeholder - "
                    "every Telegram send will fail until you fill in the real certificate.");
  }

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    startMonitoring();
  } else {
    Serial.println("WARNING: WiFi not connected at boot - cameras and the web UI will start "
                    "automatically once loop() reconnects; no reboot needed once the network is back.");
  }

  confirmFirmwareAndReportRollback();
}

// Stamps lastMs and returns true once intervalMs has passed.
static bool due(unsigned long& lastMs, unsigned long intervalMs) {
  if (millis() - lastMs < intervalMs) return false;
  lastMs = millis();
  return true;
}

static bool dueOnline(unsigned long& lastMs, unsigned long intervalMs) {
  return WiFi.status() == WL_CONNECTED && due(lastMs, intervalMs);
}

// Only loop() calls WiFi.begin(); camera tasks just read the status.
static void reconnectWifiIfDown() {
  if (WiFi.status() == WL_CONNECTED || !wifiReconnectDue()) return;
  Serial.println("\nWiFi disconnected.");
  connectWiFi();
  bool connected = WiFi.status() == WL_CONNECTED;
  recordWifiReconnectResult(connected);
  if (connected) {
    if (!g_monitoringStarted) startMonitoring();
    else setupTime();
  }
}

// Each send below can wait up to 45s for the Telegram mutex, and the 6h/1h/
// 15min intervals line up on the same tick every 6 hours. Resetting the
// watchdog after each keeps the stacked waits under its 90s timeout.
static void runPeriodicReports() {
  if (dueOnline(lastHeartbeatMs, HEARTBEAT_INTERVAL_MS)) {
    sendHeartbeat();
    esp_task_wdt_reset();
  }
  // The digest resets the counters, so exactly once per interval.
  if (dueOnline(lastDigestMs, DAILY_DIGEST_INTERVAL_MS)) {
    checkDailyActivityDigest(g_cameras.data(), g_cameraStates.data(), g_cameras.size());
    esp_task_wdt_reset();
  }
  if (dueOnline(lastNvsCheckMs, NVS_USAGE_CHECK_INTERVAL_MS)) {
    checkNvsUsage();
    esp_task_wdt_reset();
  }
  if (dueOnline(lastSdUsageCheckMs, SD_USAGE_CHECK_INTERVAL_MS)) {
    checkSdUsage();
    esp_task_wdt_reset();
  }
  if (dueOnline(lastWifiRssiCheckMs, WIFI_RSSI_CHECK_INTERVAL_MS)) {
    checkWifiSignal();
    esp_task_wdt_reset();
  }
  // Retries broadcast alerts that failed earlier (e.g. during a WAN outage).
  if (dueOnline(lastTelegramRetryFlushMs, TELEGRAM_RETRY_FLUSH_INTERVAL_MS)) {
    flushTelegramRetryQueue();
    esp_task_wdt_reset();
  }
}

// WAN reachability, beyond the local WiFi link. A relay pulse blocks for up to
// NET_WATCHDOG_PULSE_MAX_MS, hence the watchdog reset.
static void runInternetWatchdog() {
  if (!dueOnline(lastNetWatchdogCheckMs, NET_WATCHDOG_CHECK_INTERVAL_MS)) return;
  NetWatchdogCheckResult netResult = checkInternetAndMaybePulseRelay();
  if (netResult.event == NetWatchdogCheckResult::Event::OutageDetected) {
    // Likely to fail - WAN is down. The Recovered report is the reliable one.
    sendTelegramMessage([](TelegramLang lang) { return trInternetOutageAlert(lang); });
  } else if (netResult.event == NetWatchdogCheckResult::Event::Recovered) {
    String sinceTime = formatLocalClockTime(netResult.outageStartMs);
    unsigned long durationMs = netResult.outageDurationMs;
    sendTelegramMessage([sinceTime, durationMs](TelegramLang lang) {
      return trInternetRecovered(lang, sinceTime, durationMs);
    });
  }
  esp_task_wdt_reset();
}

// Both bridge cameras offline too long -> power-cycle the bridge.
static void runBridgeWatchdog() {
  if (!dueOnline(lastBridgeWatchdogCheckMs, BRIDGE_WATCHDOG_CHECK_INTERVAL_MS)) return;
  BridgeWatchdogCheckResult bridgeResult =
      checkBridgeCamerasAndMaybePulseRelay(g_cameras.data(), g_cameraStates.data(), g_cameras.size());
  if (bridgeResult.event == BridgeWatchdogCheckResult::Event::OutageDetected) {
    sendTelegramMessage([](TelegramLang lang) { return trBridgeOutageAlert(lang); });
  } else if (bridgeResult.event == BridgeWatchdogCheckResult::Event::Recovered) {
    String sinceTime = formatLocalClockTime(bridgeResult.outageStartMs);
    unsigned long durationMs = bridgeResult.outageDurationMs;
    sendTelegramMessage([sinceTime, durationMs](TelegramLang lang) {
      return trBridgeRecovered(lang, sinceTime, durationMs);
    });
  }
  esp_task_wdt_reset();
}

// No WiFi gate: debouncing must keep running through an outage.
static void runPowerMonitor() {
  if (!due(lastPowerMonitorCheckMs, POWER_MONITOR_CHECK_INTERVAL_MS)) return;
  if (checkPowerStateChanged()) {
    bool present = getPowerMonitorStatus().powerPresent;
    sendTelegramMessage([present](TelegramLang lang) { return present ? trPowerRestored(lang) : trPowerLost(lang); });
  }
}

static void runSnapshotStorageChecks() {
  // Optional full SD check (off by default). Holds the SD mutex for the whole
  // walk, briefly delaying camera writes. Clamped at use: an unclamped hour
  // count overflows the multiply above ~1193h.
  uint32_t safeSdCheckHours = sdCheckIntervalHours();
  if (safeSdCheckHours > SD_CHECK_INTERVAL_MAX_HOURS) safeSdCheckHours = SD_CHECK_INTERVAL_MAX_HOURS;
  if (WiFi.status() == WL_CONNECTED && sdActive() && safeSdCheckHours > 0 &&
      due(lastSdCheckMs, safeSdCheckHours * 3600000UL)) {
    checkSnapshotStorage();
  }

  // Independent of the check interval above, which may be off.
  if (sdActive() && due(lastRetentionCheckMs, SD_RETENTION_CHECK_INTERVAL_MS)) {
    // Re-clamped at use; imported configs bypass the form.
    uint16_t safeRetentionDays = sdRetentionDays();
    if (safeRetentionDays > SD_RETENTION_MAX_DAYS) safeRetentionDays = SD_RETENTION_MAX_DAYS;
    enforceSnapshotRetention(g_cameras, safeRetentionDays);
  }
}

// Housekeeping only: motion alerts run in the camera tasks, and everything here
// has its own interval, so a one-second tick costs them nothing.
static const uint32_t LOOP_TICK_MS = 1000;

void loop() {
  esp_task_wdt_reset(); // feed the watchdog armed in initWatchdog() - see its comment

  // Every tick, WiFi or not, so heap events during an outage still get a
  // timestamp.
  checkHeapHealth();
  // The low-heap alert can wait up to 45s on the Telegram mutex; reset here so
  // it can't stack with a later check's wait past the 90s watchdog.
  esp_task_wdt_reset();

  // Only this task may grow g_cameras (see camera_tasks.h).
  applyPendingNewCameraIfAny();
  reapStoppedCameraTasks();

  // No WiFi gate: a stall is worth logging even if the alert has to queue.
  if (g_monitoringStarted && due(lastCameraTaskCheckMs, CAMERA_TASK_CHECK_INTERVAL_MS)) {
    checkCameraTasks();
    esp_task_wdt_reset();
  }

  reconnectWifiIfDown();

  // Name lookup can block for seconds when the internet is down.
  if (g_monitoringStarted && WiFi.status() == WL_CONNECTED) {
    maintainTimeSync();
    esp_task_wdt_reset();
  }

  runPeriodicReports();
  runInternetWatchdog();
  runBridgeWatchdog();
  runPowerMonitor();

  if (dueOnline(lastCommandPollMs, TELEGRAM_COMMAND_POLL_MS)) {
    pollTelegramCommands(g_cameras.data(), g_cameraStates.data(), g_cameras.size());
  }

  runSnapshotStorageChecks();

  // Every tick, WiFi or not: a timer set before an outage should still revert
  // on time.
  checkScheduledAlertReverts(g_cameras.data(), g_cameraStates.data(), g_cameras.size());

  delay(LOOP_TICK_MS);
}
