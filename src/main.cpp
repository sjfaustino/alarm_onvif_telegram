#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <esp_task_wdt.h>
#include <esp_system.h>   // esp_reset_reason()
#include <esp_ota_ops.h>  // esp_ota_mark_app_valid_cancel_rollback()
#include <nvs_flash.h>    // nvs_get_stats() - checkNvsUsage()
#include <nvs.h>
#include <cstdlib>
#include "config.h"
#include "build_version.h"
#include "camera.h"
#include "camera_store.h"
#include "network_store.h"
#include "telegram.h"
#include "webserver.h"
#include "backoff.h"
#include "format_utils.h"
#include "event_log_store.h"
#include "camera_tasks.h"
#include "sd_store.h"

static std::vector<CameraConfig> g_cameras;
static std::vector<CameraState> g_cameraStates;
static WifiCredentials g_wifiCredentials;
static unsigned long lastHeartbeatMs = 0;
static unsigned long lastCommandPollMs = 0;
static unsigned long lastSdCheckMs = 0;
static unsigned long lastNvsCheckMs = 0;
// True once checkNvsUsage() has already alerted for the current high-usage
// stretch - re-armed (set back false) once usage drops back under
// NVS_USAGE_WARN_PERCENT, same "alert once per state transition, not every
// check" pattern as CameraState::isOffline (telegram.cpp's
// checkCameraOnlineStatus).
static bool g_nvsUsageAlerted = false;

// True once startMonitoring() has actually run - see its comment for why
// this can happen later than setup() if WiFi wasn't up yet at boot.
static bool g_monitoringStarted = false;

static void printCameraList() {
  Serial.println("\n--- Configured cameras ---");
  for (size_t i = 0; i < g_cameras.size(); i++) {
    const CameraConfig& cfg = g_cameras[i];
    Serial.printf("  [%u] %-20s %-24s %s\n",
                  (unsigned)i, cfg.name.c_str(), extractHost(cfg.deviceServiceUrl).c_str(),
                  cfg.enabled ? "enabled" : "disabled");
  }
  Serial.println("--------------------------\n");
}

// Same as printCameraList() but for Telegram: no IP, ON/OFF instead of enabled/disabled.
static String buildCameraListMessage() {
  String s = "--- Configured cameras ---\n";
  for (size_t i = 0; i < g_cameras.size(); i++) {
    const CameraConfig& cfg = g_cameras[i];
    char line[64];
    snprintf(line, sizeof(line), "  [%u] %-20s %s\n",
              (unsigned)i, cfg.name.c_str(), cfg.enabled ? "ON" : "OFF");
    s += line;
  }
  s += "--------------------------";
  return s;
}

static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000UL;

// Extra idle time between reconnect attempts, on top of connectWiFi()'s own
// ~60s (30s primary + 30s backup). Doubles per consecutive failure up to
// WIFI_RETRY_BACKOFF_MAX_MS; resets to 0 on success - avoids retrying both
// networks at a fixed cadence for the whole length of a multi-hour outage.
static const unsigned long WIFI_RETRY_BACKOFF_START_MS = 10000UL;  // extra wait after the 1st consecutive failure
static const unsigned long WIFI_RETRY_BACKOFF_MAX_MS   = 300000UL; // cap: 5 minutes between attempts
static uint8_t g_wifiFailureStreak = 0;
static unsigned long g_wifiRetryDelayMs = 0;
static unsigned long g_wifiRetryDueMs = 0; // millis() timestamp; next connectWiFi() attempt is due once reached

// TWDT timeout for the Arduino loop() task - see initWatchdog(). Comfortably
// outlasts the longest stretch loop() can go without returning to its top
// (connectWiFi() trying primary then backup, 30s each); tryConnectWiFi also
// feeds the watchdog every 500ms while polling, so this is belt-and-suspenders.
static const uint32_t WATCHDOG_TIMEOUT_MS = 90000UL;

// Attempts one network, blocking up to timeoutMs. Returns whether it connected.
static bool tryConnectWiFi(const WifiNetwork& net, unsigned long timeoutMs) {
  if (net.ssid.length() == 0) return false;
  Serial.printf("\nConnecting to WiFi \"%s\"...\n", net.ssid.c_str());
  WiFi.begin(net.ssid.c_str(), net.password.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
    esp_task_wdt_reset();
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// Arms the TWDT against loop() so a genuinely frozen main loop reboots the
// board instead of hanging forever - the gap the Telegram heartbeat can't
// cover on its own. Deliberately not armed against the per-camera tasks:
// every SOAP call already has its own HTTP_TIMEOUT_MS bound, and
// cameraSetupSequence chains several back-to-back with no safe point to
// feed a per-task watchdog without risking a false positive on a slow camera.
static void initWatchdog() {
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
static String describeResetReason() {
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

// Applies the stored static IP config, if enabled - must run after
// WiFi.mode(WIFI_STA) but before WiFi.begin(). Same config applies
// regardless of which network (primary/backup) ends up connecting. Falls
// back to DHCP if the stored values don't parse, rather than failing to
// connect at all over a config typo.
static void applyStaticIpConfig() {
  if (!g_wifiCredentials.useStaticIP) return;

  IPAddress ip, subnet, gateway, dns;
  bool ok = ip.fromString(g_wifiCredentials.staticIP) &&
            subnet.fromString(g_wifiCredentials.staticSubnet) &&
            gateway.fromString(g_wifiCredentials.staticGateway);
  if (!ok) {
    Serial.println("WARNING: static IP config incomplete/invalid - falling back to DHCP.");
    return;
  }
  if (g_wifiCredentials.staticDNS.length() == 0 || !dns.fromString(g_wifiCredentials.staticDNS)) {
    dns = gateway; // no DNS configured (or it doesn't parse) - the gateway usually doubles as one on a home LAN
  }

  WiFi.config(ip, gateway, subnet, dns);
  Serial.printf("Static IP: %s  gateway: %s  subnet: %s  DNS: %s\n",
                ip.toString().c_str(), gateway.toString().c_str(),
                subnet.toString().c_str(), dns.toString().c_str());
}

// Tries primary, then backup (if configured) on failure. If backup is what
// worked, it's promoted to primary and persisted, so future boots try
// whichever network is actually reachable first.
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  applyStaticIpConfig();

  if (tryConnectWiFi(g_wifiCredentials.primary, WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
    return;
  }

  if (g_wifiCredentials.backup.ssid.length() > 0) {
    Serial.println("Primary WiFi not reachable - trying backup...");
    if (tryConnectWiFi(g_wifiCredentials.backup, WIFI_CONNECT_TIMEOUT_MS)) {
      Serial.print("ESP32 IP: ");
      Serial.println(WiFi.localIP());
      Serial.println("Backup WiFi connected - promoting it to primary for future boots.");
      WifiNetwork oldPrimary = g_wifiCredentials.primary;
      g_wifiCredentials.primary = g_wifiCredentials.backup;
      g_wifiCredentials.backup = oldPrimary;
      if (!saveWifiCredentials(g_wifiCredentials)) {
        Serial.println("ERROR: failed to persist the promoted backup network to NVS - "
                        "this boot will still use it, but it may try primary first again after a reboot.");
      }
      return;
    }
  }

  Serial.println("ERROR: WiFi connection failed (primary" +
                  String(g_wifiCredentials.backup.ssid.length() > 0 ? " and backup" : "") + ").");
}

static void setupTime() {
  Serial.printf("Synchronizing UTC time from %s...\n", g_wifiCredentials.ntpServer.c_str());
  configTime(0, 0, g_wifiCredentials.ntpServer.c_str());
  // Must follow configTime() (does the actual esp_sntp_init()). No port
  // setting - ESP32's SNTP client hardcodes UDP port 123.
  esp_sntp_set_sync_interval(g_wifiCredentials.ntpSyncIntervalMs);

  // configTime() above set TZ to a no-op UTC form (gmtOffset=0/daylightOffset=0
  // - the system clock stays true UTC, see WifiCredentials::posixTz). This
  // overrides it with a real POSIX TZ rule if configured, affecting only
  // DST-aware local-time reads (telegram.cpp's nowTimestampString) -
  // WS-Security's timestamp reads UTC directly via gmtime_r regardless.
  if (g_wifiCredentials.posixTz.length() > 0) {
    setenv("TZ", g_wifiCredentials.posixTz.c_str(), 1);
    tzset();
    Serial.printf("Local timezone for alert captions: %s\n", g_wifiCredentials.posixTz.c_str());
  }

  struct tm timeinfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo, 1000)) {
      char buf[25];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      Serial.printf("NTP time synchronized: %s\n", buf);
      return;
    }
    Serial.print(".");
  }
  Serial.println("\nWARNING: NTP synchronization failed.");
}

// Periodic "still alive" ping - a missing heartbeat (or an unexpected boot
// message between expected ones) is the signal something's wrong.
static void sendHeartbeat() {
  String msg = "\xF0\x9F\x92\x93 Camera monitor v" + String(FIRMWARE_VERSION) + " heartbeat\n";
  msg += "Uptime: " + formatUptime(millis()) + "\n";
  // The lifetime minimum next to the current value is what reveals a slow
  // leak: dropping every heartbeat means something's leaking; flat means
  // it's just normal steady-state overhead (WiFi/mbedTLS/PsychicHttp/tasks).
  msg += "Free heap: " + String(ESP.getFreeHeap()) + " bytes (min ever: " +
         String(ESP.getMinFreeHeap()) + ")\n";
  // Same nvs_get_stats call webserver_firmware.cpp's Firmware page and
  // checkNvsUsage() below both use - see NVS_USAGE_WARN_PERCENT's own
  // comment (config.h) for why this is worth watching at all. Folded into
  // every heartbeat too, not just the proactive alert below, so a slow
  // climb toward the threshold is visible before it's actually crossed.
  nvs_stats_t nvsStats;
  if (nvs_get_stats(NULL, &nvsStats) == ESP_OK && nvsStats.total_entries > 0) {
    unsigned pct = (unsigned)((uint64_t)nvsStats.used_entries * 100 / nvsStats.total_entries);
    msg += "NVS usage: " + String(pct) + "%\n";
  }
  // subscriptionActive/isOffline are written by each camera's own task;
  // this runs on loop()'s task, so reading them needs CameraStateLock -
  // see CameraState::stateMutex.
  for (size_t i = 0; i < g_cameras.size(); i++) {
    if (!g_cameras[i].enabled) continue;
    bool subscribed, offline, alertsEnabled;
    {
      CameraStateLock lock(g_cameraStates[i]);
      subscribed = g_cameraStates[i].subscriptionActive;
      offline = g_cameraStates[i].isOffline;
      alertsEnabled = g_cameraStates[i].alertsEnabled;
    }
    msg += g_cameras[i].name + ": " + (subscribed ? "subscribed" : "NOT subscribed") +
           (offline ? " (OFFLINE)" : "") + (alertsEnabled ? "" : " (alerts OFF)") + "\n";
  }
  if (!sendTelegramMessage(msg)) {
    Serial.println("Heartbeat: Telegram send failed.");
  }
}

// Proactive counterpart to the Firmware page's own NVS-usage hint
// (webserver_firmware.cpp) - see NVS_USAGE_WARN_PERCENT's comment
// (config.h) for why this is worth alerting on at all. Alerts once on
// crossing the threshold, not every call - see g_nvsUsageAlerted's own
// comment for the re-arm rule.
static void checkNvsUsage() {
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
    sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F NVS storage is " + String(pct) +
                         "% full - this board has silently dropped writes here before once it filled "
                         "up. Check the Firmware page, and consider trimming unused cameras/Telegram "
                         "users.");
  } else {
    Serial.printf("NVS usage back under %u%% (%u%%).\n", NVS_USAGE_WARN_PERCENT, pct);
    logEvent("NVS usage back under " + String((unsigned)NVS_USAGE_WARN_PERCENT) + "% (" + String(pct) + "%)");
  }
}

// Spawns g_cameras[index]'s monitoring task - see camera_tasks.h for the
// external-linkage contract (index must already be a valid, existing slot;
// this never grows g_cameras/g_cameraStates, only starts a task for a slot
// that doesn't have one yet). Called from startMonitoring()'s boot-time
// loop below for every enabled camera, and from webserver_cameras.cpp's
// save handler for a single camera live, when an edit newly enables one
// that had no task before.
void spawnCameraTask(size_t index) {
  cameraStateInit(g_cameraStates[index]); // must run before any other task can see this camera - see camera.h
  {
    // The web server may already be live and rendering /cameras
    // concurrently by the time this runs (true for both call sites - the
    // boot loop runs after startWebServer(), and the live-add path runs
    // while the server's obviously already up) - this write needs the
    // lock too, same as any other cross-task access.
    CameraStateLock lock(g_cameraStates[index]);
    g_cameraStates[index].alertsEnabled = loadAlertEnabledPref(index); // restore any /on or /off from before a reboot
  }
  CameraTaskContext* ctx = new CameraTaskContext{&g_cameras[index], &g_cameraStates[index]};
  char taskName[16];
  snprintf(taskName, sizeof(taskName), "cam%u", (unsigned)index);
  // 10KB stack covers the SOAP String churn plus a WiFiClientSecure TLS
  // handshake with headroom - bump if stack-canary warnings show up.
  // Pinned to core 1, same as Arduino's own loopTask (CONFIG_ARDUINO_
  // RUNNING_CORE) - a reconnect burst dilutes loopTask's share via normal
  // round-robin rather than starving it, but avoids contending with
  // ESP-IDF's WiFi/BT tasks, which stay pinned to core 0 regardless and
  // would be worse to compete with directly (hits every camera, the
  // dashboard, and Telegram, not just loopTask).
  BaseType_t created =
      xTaskCreatePinnedToCore(cameraTaskFn, taskName, 10240, ctx, tskIDLE_PRIORITY + 1, nullptr, 1);
  if (created != pdPASS) {
    // Genuine memory pressure (many cameras, a large PSRAM snapshot
    // history) can make task creation itself fail - previously silent,
    // leaving this camera with no task at all, indistinguishable on the
    // dashboard from "has a task but isn't subscribed yet" unless this is
    // surfaced loudly. Not a per-camera credential/config problem, so no
    // dashboard edit fixes it - only freeing memory (or a reboot) does.
    delete ctx; // never handed to a task, so nothing else will free it
    Serial.printf("[%s] FATAL: xTaskCreatePinnedToCore failed (out of memory?) - this camera will NOT "
                  "be monitored until the board is rebooted (ideally with more free memory).\n",
                  g_cameras[index].name.c_str());
    logEvent(g_cameras[index].name + ": task creation FAILED (out of memory?) - NOT being monitored");
    sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F " + g_cameras[index].name +
                         ": failed to start its monitoring task (likely out of memory) - it is NOT "
                         "being monitored. A reboot may free enough memory to fix this.");
  }
}

// Everything that needs a working network connection: NTP, mDNS, the web
// dashboard, and one FreeRTOS task per enabled camera. Runs once, either
// right after setup()'s first successful connectWiFi(), or - if WiFi isn't
// up yet at boot - the first time loop() reconnects, so a temporary outage
// at boot delays monitoring instead of disabling it for the whole power cycle.
static void startMonitoring() {
  setupTime();

  if (MDNS.begin(g_wifiCredentials.hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: reachable at http://%s.local/\n", g_wifiCredentials.hostname.c_str());
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

  String bootMsg = "\xF0\x9F\x93\xB7 Camera monitor v" + String(FIRMWARE_VERSION) + " online\n";
  bootMsg += "Reboot reason: " + describeResetReason() + "\n";
  bootMsg += String(enabledCount) + "/" + String((int)g_cameras.size()) + " cameras enabled\n";
  bootMsg += buildCameraListMessage();
  QuickSnapshotCheckResult sdBootCheck = lastBootCheckResult();
  if (sdBootCheck.ranAtAll && !sdBootCheck.ok) {
    bootMsg += "\n\xE2\x9A\xA0\xEF\xB8\x8F SD boot check found " +
               String((unsigned)sdBootCheck.unreadableFiles) + " unreadable file(s) in " +
               String((unsigned)sdBootCheck.directoriesChecked) +
               " camera director(ies) - see the dashboard's Storage page.";
  }
  if (!sendTelegramMessage(bootMsg)) {
    Serial.println("Boot notice: Telegram send failed (network/token/CA issue?) - continuing anyway.");
  }

  lastHeartbeatMs = millis(); // first heartbeat fires HEARTBEAT_INTERVAL_MS from now, not immediately
  // Same idea for the automatic SD check (if enabled): first one fires a
  // full sdCheckIntervalHours() from now, not immediately - the boot-time
  // checkNewestSnapshots() call in initSdStorage() already just covered
  // the newest file in every camera directory.
  lastSdCheckMs = millis();
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

  g_wifiCredentials = loadWifiCredentials();

  // One-time recovery for cameras lost to the NVS migration bug fixed in
  // camera_store.cpp - adds back any secrets.h CAMERA_SEED entry missing
  // from the stored list, by name. Self-gating (see its own comment), so
  // safe to leave in permanently rather than reverting after this recovery.
  restoreMissingCamerasFromSeed();

  // Loaded once and never resized/reallocated afterward - CameraState::user/pass
  // and every CameraTaskContext hold raw pointers into these vectors' elements.
  g_cameras = loadCameras();
  g_cameraStates.resize(g_cameras.size());
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
  esp_err_t rollbackErr = esp_ota_mark_app_valid_cancel_rollback();
  if (rollbackErr == ESP_OK) {
    Serial.println("OTA rollback: this firmware confirmed healthy - won't auto-revert on the next reboot.");
  }
  // Any other result (e.g. no rollback pending - the normal case except
  // right after a firmware update) is expected, not logged as an error.
}

void loop() {
  esp_task_wdt_reset(); // feed the watchdog armed in initWatchdog() - see its comment

  // loop() alone owns WiFi connect/reconnect - camera tasks only ever read
  // WiFi.status(), never call WiFi.begin(). g_wifiRetryDueMs is the backoff
  // gate (see WIFI_RETRY_BACKOFF_START_MS above).
  if (WiFi.status() != WL_CONNECTED && (long)(millis() - g_wifiRetryDueMs) >= 0) {
    Serial.println("\nWiFi disconnected.");
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      g_wifiFailureStreak = 0;
      g_wifiRetryDelayMs = 0;
      // First successful connection ever (WiFi wasn't up yet at boot) -
      // run the full deferred startup instead of just resyncing the clock.
      if (!g_monitoringStarted) startMonitoring();
      else setupTime();
    } else {
      g_wifiRetryDelayMs = nextBackoffDelayMs(g_wifiRetryDelayMs, WIFI_RETRY_BACKOFF_START_MS,
                                              WIFI_RETRY_BACKOFF_MAX_MS);
      g_wifiFailureStreak++;
      g_wifiRetryDueMs = millis() + g_wifiRetryDelayMs;
      Serial.printf("WiFi still down after %u consecutive attempt(s) - next attempt in %lus.\n",
                    (unsigned)g_wifiFailureStreak, g_wifiRetryDelayMs / 1000UL);
    }
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = millis();
    sendHeartbeat();
  }

  // NVS usage doesn't need WiFi to check (it's a local flash read), but
  // does need it to actually send the alert - gated the same way as every
  // other WiFi-dependent periodic check here, rather than checking without
  // WiFi and queuing/dropping the send.
  if (WiFi.status() == WL_CONNECTED && millis() - lastNvsCheckMs >= NVS_USAGE_CHECK_INTERVAL_MS) {
    lastNvsCheckMs = millis();
    checkNvsUsage();
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
  // motionWatchdogHours (telegram.cpp's checkMotionWatchdog).
  uint32_t safeSdCheckHours = sdCheckIntervalHours();
  if (safeSdCheckHours > SD_CHECK_INTERVAL_MAX_HOURS) safeSdCheckHours = SD_CHECK_INTERVAL_MAX_HOURS;
  if (WiFi.status() == WL_CONNECTED && sdActive() && safeSdCheckHours > 0 &&
      millis() - lastSdCheckMs >= safeSdCheckHours * 3600000UL) {
    lastSdCheckMs = millis();
    checkSnapshotStorage();
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
