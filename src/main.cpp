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
static unsigned long lastTelegramRetryFlushMs = 0;

// True once startMonitoring() has actually run - see its comment for why
// this can happen later than setup() if WiFi wasn't up yet at boot.
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

// Same as printCameraList() but for Telegram: no IP, ON/OFF instead of enabled/disabled.
static String buildCameraListMessage(TelegramLang lang) {
  bool pt = lang == TelegramLang::Portuguese;
  String s = trConfiguredCamerasHeader(lang) + "\n";
  for (size_t i = 0; i < g_cameras.size(); i++) {
    const CameraConfig& cfg = g_cameras[i];
    char line[64];
    // Checkmark for enabled, language-neutral - a column of these is a
    // faster "does anything need attention" scan than reading "ON"/
    // "LIGADA" repeated down the whole list. A disabled camera stays
    // plain text, since that's the exception actually worth reading, not
    // skimming past.
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

// Everything that needs a working network connection: NTP, mDNS, the web
// dashboard, and one FreeRTOS task per enabled camera. Runs once, either
// right after setup()'s first successful connectWiFi(), or - if WiFi isn't
// up yet at boot - the first time loop() reconnects, so a temporary outage
// at boot delays monitoring instead of disabling it for the whole power cycle.
static void startMonitoring() {
  setupTime();

  // Re-sanitized here, at the point of use, not just at the dashboard save
  // (webserver_network.cpp's handleSaveNetwork already does this) - same
  // "hand-edited/imported NVS blob bypasses the form entirely" reasoning
  // as this project's numeric config clamps: config Import
  // (config_backup.cpp) writes WifiCredentials::hostname straight
  // from an uploaded file via saveWifiCredentials, with no filtering of
  // its own. An unsanitized character here wouldn't crash anything - it
  // would just silently fail to resolve (see sanitizeHostname's own
  // comment, network_store.h) - the existing failure branch below already
  // covers "resulted in nothing usable".
  String safeHostname = sanitizeHostname(g_wifiCredentials.hostname);
  if (MDNS.begin(safeHostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: reachable at http://%s.local/\n", safeHostname.c_str());
  } else {
    Serial.println("WARNING: mDNS.begin() failed - the .local hostname won't resolve; "
                    "the IP address still works.");
  }

  startWebServer(&g_cameras, &g_cameraStates); // logs its own "listening on http://<ip>:80/" line

  // One FreeRTOS task per enabled camera, pinned to core 1 - see
  // spawnCameraTask below. The delay() after each spawn staggers initial
  // GetCapabilities calls: several cameras' embedded HTTP stacks only
  // accept 1-2 concurrent connections, so hitting all N at once caused a
  // thundering herd of "Connection reset by peer" at boot.
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

  // Logged here, not earlier in setup() - the event log (event_log_store.h)
  // is itself purely in-RAM and wiped by the very reboot a /reset or OTA
  // update causes, so logging *those* would never actually be seen; this
  // boot line is the one event in this project's whole event-log wiring
  // that's guaranteed to survive into the new session, and doubles as the
  // "did it actually restart" marker for whichever of them just happened.
  logEvent("Booted: " + describeResetReason());

  QuickSnapshotCheckResult sdBootCheck = lastBootCheckResult();
  bool sdBootCheckFailed = sdBootCheck.ranAtAll && !sdBootCheck.ok;
  size_t sdBootUnreadable = sdBootCheck.unreadableFiles;
  size_t sdBootDirsChecked = sdBootCheck.directoriesChecked;
  bool powerMonitorEnabled = powerMonitorActive();
  bool powerPresentAtBoot = getPowerMonitorStatus().powerPresent;
  // Distinct from sdBootCheckFailed above (that's a readability check on
  // snapshots SD already has) - this is SD storage being enabled but never
  // having mounted at boot at all (initSdStorage/sd_store.cpp couldn't
  // send this itself - no network yet at that point in setup()), same
  // "fold a boot-time condition into this one notice" pattern as
  // powerMonitorEnabled/powerPresentAtBoot above.
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
  // Same idea for the automatic SD check (if enabled): first one fires a
  // full sdCheckIntervalHours() from now, not immediately - the boot-time
  // checkNewestSnapshots() call in initSdStorage() already just covered
  // the newest file in every camera directory.
  lastSdCheckMs = millis();
  lastRetentionCheckMs = millis(); // first sweep fires SD_RETENTION_CHECK_INTERVAL_MS from now
  g_monitoringStarted = true;
}

void setup() {
  Serial.begin(115200);
  delay(5000); // time to open the serial terminal before the boot log starts scrolling
  Serial.println("\n========================================");
  Serial.println("MULTI-CAMERA ONVIF MOTION MONITOR");
  Serial.println("========================================");
  Serial.printf("Reboot reason: %s\n", describeResetReason().c_str());

  Serial.printf("PSRAM: %u bytes%s\n", (unsigned)ESP.getPsramSize(),
                ESP.getPsramSize() == 0 ? " (none detected)" : "");

  // PSRAM is mandatory: triggerMotionAlert buffers the full snapshot in RAM
  // once to resend per Telegram recipient. Without PSRAM that buffer would
  // compete with mbedTLS's own TLS session buffers in internal RAM - the
  // kind of thing that fails partway through a send rather than at boot, so
  // refusing to start is safer than trusting a board likely to run out.
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

  // Routes any single allocation >=4KB to PSRAM automatically (e.g. the
  // multi-KB SOAP response Strings in camera.cpp), keeping them off internal
  // RAM where they'd compete with mbedTLS's TLS buffers. Small/frequent
  // allocations stay internal on purpose - PSRAM is slower per-access.
  heap_caps_malloc_extmem_enable(4096);
  Serial.println("PSRAM: allocations >=4KB will be routed here automatically.");

  // After the PSRAM gate, not before - that halt loop is a deliberate,
  // permanent refusal to boot, not a hang the watchdog should reboot out of.
  initWatchdog();

  // Optional - does nothing at all unless the Storage page's setting is
  // enabled (see sd_store.h). Must run before camera tasks start (below),
  // since sdActive() needs to be settled before the first snapshot could
  // possibly be pushed.
  initSdStorage();

  // Optional - does nothing at all unless the Network page's RTC setting
  // is enabled (see rtc_store.h). Runs, and seeds the system clock, before
  // connectWiFi()/setupTime() below - the whole reason for an external RTC
  // is having a roughly-correct clock available even before WiFi/NTP have
  // had any chance to run (e.g. an extended WiFi outage after boot).
  initRtc();
  seedSystemClockFromRtc();

  // Optional - does nothing at all unless the Hardware > Internet page's
  // setting is enabled (see net_watchdog.h). Sets the relay to its
  // resting ("router powered") state immediately, before WiFi/monitoring
  // start - it should never sit in the "just pulsed" state across a reboot.
  initNetWatchdog();

  // Same idea, second independent relay - see bridge_watchdog.h. Called
  // after initNetWatchdog() so its own pin-conflict check sees the
  // Internet Watchdog's already-loaded settings.
  initBridgeWatchdog();

  // Third independent relay/sensor feature - see power_monitor.h. Called
  // after the other two so its own pin-conflict check sees both of their
  // already-loaded settings.
  initPowerMonitor();

  g_wifiCredentials = loadWifiCredentials();

  // One-time recovery for cameras lost to the NVS migration bug fixed in
  // camera_store.cpp - adds back any secrets.h CAMERA_SEED entry missing
  // from the stored list, by name. Self-gating (see its own comment), so
  // safe to leave in permanently rather than reverting after this recovery.
  restoreMissingCamerasFromSeed();

  // Sized to the real camera count here, then reserved up to MAX_CAMERAS
  // (config.h) below - CameraState::user/pass and every CameraTaskContext
  // hold raw pointers into these vectors' elements, so nothing may ever
  // reallocate them once a task exists. reserve() right after this, still
  // before any task is spawned, is what makes it safe for
  // applyPendingNewCameraIfAny (camera_tasks.h) to push_back a brand-new
  // camera live later without invalidating those pointers - see its own
  // comment for the full reasoning.
  g_cameras = loadCameras();
  g_cameraStates.resize(g_cameras.size());
  // A no-op if loadCameras() already returned more than MAX_CAMERAS
  // entries (capacity is already at least that many) - reserve() never
  // shrinks a vector, only grows its unused headroom.
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

  // Last, and not gated on WiFi - see its own comment (boot_checks.cpp).
  confirmFirmwareAndReportRollback();
}

void loop() {
  esp_task_wdt_reset(); // feed the watchdog armed in initWatchdog() - see its comment

  // Every tick, not gated behind an interval or WiFi.status() - see
  // checkHeapHealth's own comment for why that's cheap, and
  // checkScheduledAlertReverts below for the same "runs regardless of WiFi"
  // reasoning (a heap event during an outage should still get a timestamp).
  checkHeapHealth();
  // checkHeapHealth's one-time low-heap alert can block on
  // telegram_transport.cpp's g_telegramNetMutex for up to TELEGRAM_NET_MUTEX_TIMEOUT_MS
  // (45s) before sendTelegramMessage's own per-recipient reset ever fires -
  // same reasoning as the reset after each of sendHeartbeat/checkNvsUsage/
  // checkWifiSignal below: without this, that alert (rare, but its timing
  // is uncorrelated with theirs) landing on the same tick as one of those
  // could stack two uncovered ~45s gaps toward WATCHDOG_TIMEOUT_MS (90s).
  esp_task_wdt_reset();

  // Cheap when nothing's staged (a mutex take/give and a null check) - see
  // camera_tasks.h's own comment for why this specific operation (growing
  // g_cameras/g_cameraStates for a brand-new camera) must happen only on
  // this task, unlike every other camera-lifecycle action the dashboard
  // can trigger live. Doesn't need WiFi - spawning a task here is no
  // different from the boot-time loop below doing it with WiFi already
  // confirmed connected; a camera task added while WiFi is briefly down
  // just retries with its own normal backoff, same as any other.
  applyPendingNewCameraIfAny();

  // loop() alone owns WiFi connect/reconnect - camera tasks only ever read
  // WiFi.status(), never call WiFi.begin(). wifiReconnectDue() is the backoff
  // gate (see WIFI_RETRY_BACKOFF_START_MS, wifi_connect.cpp).
  if (WiFi.status() != WL_CONNECTED && wifiReconnectDue()) {
    Serial.println("\nWiFi disconnected.");
    connectWiFi();
    bool connected = WiFi.status() == WL_CONNECTED;
    recordWifiReconnectResult(connected);
    if (connected) {
      // First successful connection ever (WiFi wasn't up yet at boot) -
      // run the full deferred startup instead of just resyncing the clock.
      if (!g_monitoringStarted) startMonitoring();
      else setupTime();
    }
  }

  // Every one of these three can block on telegram_transport.cpp's g_telegramNetMutex
  // for up to TELEGRAM_NET_MUTEX_TIMEOUT_MS (45s) before a send is even
  // attempted - and HEARTBEAT_INTERVAL_MS/NVS_USAGE_CHECK_INTERVAL_MS/
  // WIFI_RSSI_CHECK_INTERVAL_MS (6h/1h/15min) are exact multiples of each
  // other, so all three land on the SAME loop() tick every 6 hours by
  // construction, not just by bad luck. None of sendHeartbeat/
  // checkNvsUsage/checkWifiSignal feed the task watchdog mid-call the way
  // handleAllCamerasCommand's "/snap all" loop already does - without a
  // reset between them here, three stacked worst-case waits in one tick
  // could approach or exceed WATCHDOG_TIMEOUT_MS (90s, initWatchdog()) and
  // panic-reboot the board over nothing but timing.
  if (WiFi.status() == WL_CONNECTED && millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = millis();
    sendHeartbeat();
    esp_task_wdt_reset();
  }

  // Same cadence style as the heartbeat above, independent interval -
  // checkDailyActivityDigest resets every camera's counters the moment
  // it's called, so this must only run once per DAILY_DIGEST_INTERVAL_MS,
  // never every tick.
  if (WiFi.status() == WL_CONNECTED && millis() - lastDigestMs >= DAILY_DIGEST_INTERVAL_MS) {
    lastDigestMs = millis();
    checkDailyActivityDigest(g_cameras.data(), g_cameraStates.data(), g_cameras.size());
    esp_task_wdt_reset();
  }

  // NVS usage doesn't need WiFi to check (it's a local flash read), but
  // does need it to actually send the alert - gated the same way as every
  // other WiFi-dependent periodic check here, rather than checking without
  // WiFi and queuing/dropping the send.
  if (WiFi.status() == WL_CONNECTED && millis() - lastNvsCheckMs >= NVS_USAGE_CHECK_INTERVAL_MS) {
    lastNvsCheckMs = millis();
    checkNvsUsage();
    esp_task_wdt_reset();
  }

  // Same "doesn't need WiFi to check, does need it to alert" reasoning as
  // checkNvsUsage above - checkSdUsage() itself no-ops instantly if SD
  // isn't active, so this costs nothing on a board without SD storage.
  if (WiFi.status() == WL_CONNECTED && millis() - lastSdUsageCheckMs >= SD_USAGE_CHECK_INTERVAL_MS) {
    lastSdUsageCheckMs = millis();
    checkSdUsage();
    esp_task_wdt_reset();
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastWifiRssiCheckMs >= WIFI_RSSI_CHECK_INTERVAL_MS) {
    lastWifiRssiCheckMs = millis();
    checkWifiSignal();
    esp_task_wdt_reset();
  }

  // Retries any systemMessages broadcast alert that failed to send earlier
  // (most likely queued during a WAN outage this same loop's own Internet
  // Watchdog check is tracking) - gated the same way as every other
  // WiFi-dependent periodic check here, since there's no point attempting
  // otherwise.
  if (WiFi.status() == WL_CONNECTED && millis() - lastTelegramRetryFlushMs >= TELEGRAM_RETRY_FLUSH_INTERVAL_MS) {
    lastTelegramRetryFlushMs = millis();
    flushTelegramRetryQueue();
    esp_task_wdt_reset();
  }

  // WiFi.status()==WL_CONNECTED only confirms the board's own link to the
  // router - this goes one hop further (real WAN reachability), which is
  // the whole point for a board sitting behind a 4G/LTE router that can
  // lose its uplink while that local link stays up the whole time. Gated
  // the same way as every other WiFi-dependent check here; the relay
  // pulse (when it happens) blocks for up to NET_WATCHDOG_PULSE_MAX_MS, so
  // the watchdog reset matters here too, same reasoning as the comment
  // above checkNvsUsage's own call site.
  if (WiFi.status() == WL_CONNECTED && millis() - lastNetWatchdogCheckMs >= NET_WATCHDOG_CHECK_INTERVAL_MS) {
    lastNetWatchdogCheckMs = millis();
    NetWatchdogCheckResult netResult = checkInternetAndMaybePulseRelay();
    if (netResult.event == NetWatchdogCheckResult::Event::OutageDetected) {
      // Best-effort only - WAN connectivity is confirmed absent right
      // now, the exact thing this send itself needs, so it's likely to
      // fail silently. That's what the Recovered case below is for.
      sendTelegramMessage([](TelegramLang lang) { return trInternetOutageAlert(lang); });
    } else if (netResult.event == NetWatchdogCheckResult::Event::Recovered) {
      // Sent once WAN is confirmed back, so - unlike the OutageDetected
      // alert above - this one actually has a working path to deliver
      // over. formatLocalClockTime tolerates a past timestamp fine (see
      // its own comment) even though it reads as future-tense ("due").
      String sinceTime = formatLocalClockTime(netResult.outageStartMs);
      unsigned long durationMs = netResult.outageDurationMs;
      sendTelegramMessage([sinceTime, durationMs](TelegramLang lang) {
        return trInternetRecovered(lang, sinceTime, durationMs);
      });
    }
    esp_task_wdt_reset();
  }

  // Same relay-power-cycle idea, different failure signal: two specific
  // configured cameras (Hardware > WiFi Bridge page) both offline for too
  // long points at the local wireless bridge carrying them, not either
  // camera itself - see bridge_watchdog.h. WiFi.status() gate kept for
  // consistency with every other periodic block here, even though the
  // offline check itself doesn't need WAN - sendTelegramMessage does.
  if (WiFi.status() == WL_CONNECTED && millis() - lastBridgeWatchdogCheckMs >= BRIDGE_WATCHDOG_CHECK_INTERVAL_MS) {
    lastBridgeWatchdogCheckMs = millis();
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

  // 220V mains power monitor - see power_monitor.h. No WiFi.status() gate
  // on the check itself (a digitalRead needs no network, and the
  // debounce state must keep tracking correctly through a WiFi outage so
  // the eventual alert isn't wrong/duplicated once it reconnects) -
  // sendTelegramMessage still just no-ops/logs if WiFi happens to be down
  // right when a change is confirmed, same as checkScheduledAlertReverts'
  // own reasoning.
  if (millis() - lastPowerMonitorCheckMs >= POWER_MONITOR_CHECK_INTERVAL_MS) {
    lastPowerMonitorCheckMs = millis();
    if (checkPowerStateChanged()) {
      bool present = getPowerMonitorStatus().powerPresent;
      sendTelegramMessage([present](TelegramLang lang) { return present ? trPowerRestored(lang) : trPowerLost(lang); });
    }
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastCommandPollMs >= TELEGRAM_COMMAND_POLL_MS) {
    lastCommandPollMs = millis();
    pollTelegramCommands(g_cameras.data(), g_cameraStates.data(), g_cameras.size());
  }

  // Automatic full SD storage check - off by default (sdCheckIntervalHours()
  // == 0), opt-in via the Storage page. Runs from loop(), not a dedicated
  // task - background/webserver-tier concern, not camera-critical.
  // checkSnapshotStorage() holds the SD mutex for its entire walk, so a
  // large history can briefly delay a camera's own SD write while this
  // runs - same tradeoff the manual "check storage" button always had.
  // Clamped here, at the point of use, not just at the dashboard save
  // (which clamps to SD_CHECK_INTERVAL_MAX_HOURS=720) - loadSdSettings
  // applies no clamp of its own, so a hand-edited NVS blob could hold a
  // value large enough to overflow the *3600000UL multiply (wraps above
  // ~1193 hours), same overflow class already fixed for
  // motionWatchdogHours (telegram_alerts.cpp's checkMotionWatchdog).
  uint32_t safeSdCheckHours = sdCheckIntervalHours();
  if (safeSdCheckHours > SD_CHECK_INTERVAL_MAX_HOURS) safeSdCheckHours = SD_CHECK_INTERVAL_MAX_HOURS;
  if (WiFi.status() == WL_CONNECTED && sdActive() && safeSdCheckHours > 0 &&
      millis() - lastSdCheckMs >= safeSdCheckHours * 3600000UL) {
    lastSdCheckMs = millis();
    checkSnapshotStorage();
  }

  // Snapshot retention sweep - runs on its own daily cadence, independent
  // of the storage-check dial above (that one can legitimately be off
  // while retention still needs to run). sdActive() gate matches every
  // other SD operation here; enforceSnapshotRetention() itself no-ops the
  // walk per camera whose effective retention is 0 ("keep forever").
  if (sdActive() && millis() - lastRetentionCheckMs >= SD_RETENTION_CHECK_INTERVAL_MS) {
    lastRetentionCheckMs = millis();
    // Re-clamped here, at the point of use, not just at the dashboard save
    // (which already clamps to SD_RETENTION_MAX_DAYS) - same "hand-edited/
    // imported NVS blob bypasses the form entirely" reasoning as
    // safeSdCheckHours above.
    uint16_t safeRetentionDays = sdRetentionDays();
    if (safeRetentionDays > SD_RETENTION_MAX_DAYS) safeRetentionDays = SD_RETENTION_MAX_DAYS;
    enforceSnapshotRetention(g_cameras, safeRetentionDays);
  }

  // Every tick, not gated behind its own interval like the poll above -
  // see checkScheduledAlertReverts' own comment for why that's cheap.
  // Runs regardless of WiFi status too: a timer set before an outage
  // should still revert on schedule even if Telegram can't be reached
  // right at that instant (sendTelegramMessage inside it just fails/logs,
  // same as any other broadcast during an outage).
  checkScheduledAlertReverts(g_cameras.data(), g_cameraStates.data(), g_cameras.size());

  delay(1000);
}
