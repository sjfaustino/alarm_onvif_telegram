#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "camera.h"
#include "camera_store.h"
#include "network_store.h"
#include "telegram.h"
#include "webserver.h"

static std::vector<CameraConfig> g_cameras;
static std::vector<CameraState> g_cameraStates;
static WifiCredentials g_wifiCredentials;
static unsigned long lastHeartbeatMs = 0;
static unsigned long lastCommandPollMs = 0;

// True once startMonitoring() has actually run - see its comment for why
// this can happen later than setup() if WiFi wasn't up yet at boot.
static bool g_monitoringStarted = false;

// Extracts "host[:port]" out of a URL like "http://192.168.1.178:8899/onvif/..."
// for the startup camera listing.
static String extractHost(const String& url) {
  int schemeEnd = url.indexOf("://");
  int start = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', start);
  int end = (pathStart >= 0) ? pathStart : (int)url.length();
  return url.substring(start, end);
}

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

// Same layout as printCameraList(), but for the Telegram boot message:
// no IP (not useful/actionable to a phone reader) and ON/OFF instead of
// enabled/disabled so it reads at a glance.
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

// Extra idle time between reconnect attempts once WiFi has been down for a
// while - on top of connectWiFi()'s own up to ~60s (30s primary + 30s
// backup) per attempt, not a replacement for it. Doubles on each
// consecutive failure up to WIFI_RETRY_BACKOFF_MAX_MS, and resets to 0 the
// moment a reconnect succeeds. Without this, loop() retried both networks
// back-to-back forever at a fixed ~61s cadence for the entire length of an
// outage - harmless for a few minutes, but needlessly persistent (and, on
// some networks, the kind of steady repeated-auth-attempt pattern that can
// trigger a router's own lockout) over an outage lasting hours.
static const unsigned long WIFI_RETRY_BACKOFF_START_MS = 10000UL;  // extra wait after the 1st consecutive failure
static const unsigned long WIFI_RETRY_BACKOFF_MAX_MS   = 300000UL; // cap: 5 minutes between attempts
static uint8_t g_wifiFailureStreak = 0;
static unsigned long g_wifiRetryDelayMs = 0;
static unsigned long g_wifiRetryDueMs = 0; // millis() timestamp; next connectWiFi() attempt is due once reached

// TWDT timeout for the Arduino loop() task - see initWatchdog()'s comment
// for why this only watches loop(), not the per-camera tasks. Sized to
// comfortably outlast the longest legitimate stretch loop() can go without
// returning to its top: connectWiFi() trying primary then backup, 30s each
// (tryConnectWiFi also feeds the watchdog every 500ms while it polls, so
// this margin is belt-and-suspenders, not the only thing standing between
// a real WiFi outage and a false-positive reboot).
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

// Arms the ESP32's Task Watchdog Timer against loop() (the Arduino
// "loopTask", which setup() also runs on) so a genuinely frozen main loop
// reboots the board instead of sitting there forever - the gap the
// Telegram heartbeat can't cover on its own, since a hung loop() can't
// send anything either. Deliberately NOT armed against the per-camera
// tasks (camera.cpp's cameraTaskFn): every SOAP call they make already has
// its own HTTP_TIMEOUT_MS bound (see onvif_soap.cpp), and cameraSetupSequence
// chains several of those calls back-to-back without returning to a point
// where a per-task watchdog could safely be fed without risking a
// false-positive reboot on a merely slow (not hung) camera.
static void initWatchdog() {
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // keep watching both cores' idle tasks too
    .trigger_panic = true,
  };
  // Recent Arduino-ESP32 cores already init the TWDT by default (for the
  // idle tasks) - esp_task_wdt_init() fails with ESP_ERR_INVALID_STATE in
  // that case, so fall back to reconfiguring the already-running one
  // instead of treating that as an error.
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

// Applies g_wifiCredentials' static IP config, if enabled, via WiFi.config()
// - must happen after WiFi.mode(WIFI_STA) but before WiFi.begin() to take
// effect. Same static config is used regardless of which network (primary
// or backup) ends up connecting - a static setup is typically two APs
// sharing one LAN/subnet, not two independent networks. Falls back to DHCP
// (by simply not calling WiFi.config()) if the stored values don't parse as
// IP addresses, rather than failing to connect at all over a config typo.
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

// Tries the primary network first; if it doesn't connect within
// WIFI_CONNECT_TIMEOUT_MS and a backup is configured, tries that instead.
// If the backup is what actually worked, it's promoted to primary (and the
// old primary demoted to backup) and persisted to NVS, so the next boot -
// and every reconnect attempt from loop() until then - tries the network
// that's actually reachable first instead of wasting 30s on the one that
// isn't.
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
      saveWifiCredentials(g_wifiCredentials);
      return;
    }
  }

  Serial.println("ERROR: WiFi connection failed (primary" +
                  String(g_wifiCredentials.backup.ssid.length() > 0 ? " and backup" : "") + ").");
}

static void setupTime() {
  Serial.printf("Synchronizing UTC time from %s...\n", g_wifiCredentials.ntpServer.c_str());
  configTime(0, 0, g_wifiCredentials.ntpServer.c_str());
  // Must come after configTime() (which does the actual esp_sntp_init())
  // rather than before - adjusts the interval used for every resync after
  // this first one. No port setting exists here on purpose - ESP32's SNTP
  // client hardcodes the standard NTP UDP port 123.
  esp_sntp_set_sync_interval(g_wifiCredentials.ntpSyncIntervalMs);

  struct tm timeinfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo, 1000)) {
      Serial.println("NTP time synchronized.");
      return;
    }
    Serial.print(".");
  }
  Serial.println("\nWARNING: NTP synchronization failed.");
}

static String formatUptime(unsigned long ms) {
  unsigned long totalSec = ms / 1000UL;
  unsigned long days  = totalSec / 86400UL;
  unsigned long hours = (totalSec % 86400UL) / 3600UL;
  unsigned long mins  = (totalSec % 3600UL) / 60UL;
  String s;
  if (days > 0) s += String(days) + "d ";
  s += String(hours) + "h " + String(mins) + "m";
  return s;
}

// Periodic "still alive" ping - the boot message alone doesn't help you
// notice the board has been silently hung or unexpectedly rebooted for the
// last N hours; if a heartbeat goes missing (or an unexpected boot message
// shows up between expected heartbeats), that's the signal something's
// wrong even though nothing else in the log would tell you. Doesn't detect
// a fully frozen board on its own (that needs loop() itself to keep
// running) - pair with a hardware/task watchdog if that's a real risk for
// your setup.
static void sendHeartbeat() {
  String msg = "\xF0\x9F\x92\x93 Camera monitor heartbeat\n";
  msg += "Uptime: " + formatUptime(millis()) + "\n";
  msg += "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
  for (size_t i = 0; i < g_cameras.size(); i++) {
    if (!g_cameras[i].enabled) continue;
    msg += g_cameras[i].name + ": " +
           (g_cameraStates[i].subscriptionActive ? "subscribed" : "NOT subscribed") +
           (g_cameraStates[i].isOffline ? " (OFFLINE)" : "") +
           (g_cameraStates[i].alertsEnabled ? "" : " (alerts OFF)") + "\n";
  }
  if (!sendTelegramMessage(msg)) {
    Serial.println("Heartbeat: Telegram send failed.");
  }
}

// Everything that needs a working network connection: NTP, mDNS, the web
// dashboard, and one FreeRTOS task per enabled camera. Normally runs once,
// right after setup()'s first successful connectWiFi(). If WiFi *isn't* up
// yet at boot (e.g. the router lost power at the same time and hasn't come
// back), setup() skips this instead of blocking indefinitely - loop() then
// calls it the first time connectWiFi() succeeds, so a temporary outage at
// boot delays monitoring instead of disabling it for the rest of the
// power cycle (which is what happened before this existed: setup() used to
// inline all of this after an early `return` on WiFi failure, and loop()
// only ever retried the WiFi connection itself, never came back to start
// the cameras or web server).
static void startMonitoring() {
  setupTime();

  if (MDNS.begin(g_wifiCredentials.hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: reachable at http://%s.local/\n", g_wifiCredentials.hostname.c_str());
  } else {
    Serial.println("WARNING: mDNS.begin() failed - the .local hostname won't resolve; "
                    "the IP address still works.");
  }

  startWebServer(&g_cameras, &g_cameraStates);
  Serial.printf("Web UI: http://%s/\n", WiFi.localIP().toString().c_str());

  // One FreeRTOS task per enabled camera, pinned to core 1 - see
  // xTaskCreatePinnedToCore's comment below and cameraTaskFn's comment in
  // camera.cpp for why (this board turned out to be a genuine dual-core
  // ESP32, so these run in true parallel, not just time-sliced). Each task
  // does its own initial cameraSetupSequence before entering its loop, so
  // this just needs to spawn them.
  //
  // The delay() after each spawn staggers those initial GetCapabilities
  // calls instead of firing all of them within the same handful of
  // milliseconds. Several of these cameras' embedded HTTP stacks only
  // accept 1-2 concurrent connections, so hitting all N at once caused a
  // thundering-herd of "Connection reset by peer" failures at boot even
  // though the cameras were fine individually.
  for (size_t i = 0; i < g_cameras.size(); i++) {
    if (!g_cameras[i].enabled) {
      Serial.printf("[%s] Disabled - no task created.\n", g_cameras[i].name.c_str());
      continue;
    }
    g_cameraStates[i].alertsEnabled = loadAlertEnabledPref(i); // restore any /on or /off from before a reboot
    CameraTaskContext* ctx = new CameraTaskContext{&g_cameras[i], &g_cameraStates[i]};
    char taskName[16];
    snprintf(taskName, sizeof(taskName), "cam%u", (unsigned)i);
    // 10KB stack: covers the SOAP String churn plus a WiFiClientSecure TLS
    // handshake and the 2KB streaming chunk buffer from telegram.cpp with
    // headroom. Bump this if you see stack-canary warnings in the log.
    //
    // Pinned to core 1: this board is a genuine dual-core ESP32 (confirmed
    // via esptool chip-id - see platformio.ini), not the single-core S2
    // this was first written against. Core 0 runs the WiFi/BT stack tasks
    // and the Arduino loopTask; pinning camera tasks to core 1 gives them
    // real parallel execution instead of competing with WiFi internals for
    // core 0's time slices.
    xTaskCreatePinnedToCore(cameraTaskFn, taskName, 10240, ctx, tskIDLE_PRIORITY + 1, nullptr, 1);
    delay(750); // stagger initial GetCapabilities calls - see comment above
  }

  int enabledCount = 0;
  for (size_t i = 0; i < g_cameras.size(); i++) if (g_cameras[i].enabled) enabledCount++;

  String bootMsg = "\xF0\x9F\x93\xB7 Camera monitor online\n";
  bootMsg += String(enabledCount) + "/" + String((int)g_cameras.size()) + " cameras enabled\n";
  bootMsg += buildCameraListMessage();
  if (!sendTelegramMessage(bootMsg)) {
    Serial.println("Boot notice: Telegram send failed (network/token/CA issue?) - continuing anyway.");
  }

  lastHeartbeatMs = millis(); // first heartbeat fires HEARTBEAT_INTERVAL_MS from now, not immediately
  g_monitoringStarted = true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("MULTI-CAMERA ONVIF MOTION MONITOR");
  Serial.println("========================================");

  Serial.printf("PSRAM: %u bytes%s\n", (unsigned)ESP.getPsramSize(),
                ESP.getPsramSize() == 0 ? " (none detected)" : "");

  // PSRAM is mandatory, not just preferred - triggerMotionAlert (telegram.cpp)
  // always buffers the full snapshot in RAM now, once, to resend it to every
  // Telegram user subscribed to that camera. On a no-PSRAM board that buffer
  // (up to SNAPSHOT_MAX_BYTES) would sit in internal RAM alongside mbedTLS's
  // own fixed ~32KB of TLS session buffers - risky, and the kind of thing
  // that fails partway through a send rather than at boot. Refusing to start
  // at all is safer than trusting a board that's likely to run out of heap
  // the first time it actually needs to.
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

  // Previously only telegram.cpp's explicit snapshot buffer used PSRAM -
  // every other sizeable allocation (GetProfiles/GetSnapshotUri SOAP
  // response Strings in onvif_soap.cpp/camera.cpp, which can run several KB
  // with multiple profiles) still competed with mbedTLS's own internal-heap
  // TLS buffers for internal RAM. This routes any single allocation >=4KB to
  // PSRAM automatically instead. Small/frequent allocations stay on internal
  // RAM on purpose - PSRAM access is slower than internal RAM, so forcing
  // every short-lived tag-name String through it would cost more than it saves.
  heap_caps_malloc_extmem_enable(4096);
  Serial.println("PSRAM: allocations >=4KB will be routed here automatically.");

  // Armed after the PSRAM gate, not before - that halt loop above is a
  // deliberate, permanent refusal to run on unsupported hardware, not a
  // hang, and arming the watchdog first would just turn it into a pointless
  // reboot-every-90s loop on a condition that will never resolve itself.
  initWatchdog();

  g_wifiCredentials = loadWifiCredentials();

  // Loaded once here and kept alive for the process's lifetime (never
  // resized/reallocated afterward) - camera.h's CameraState::user/pass and
  // every CameraTaskContext hold raw pointers/references into these
  // vectors' elements, which stay valid only as long as that holds.
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
}

void loop() {
  esp_task_wdt_reset(); // feed the watchdog armed in initWatchdog() - see its comment

  // loop() (the Arduino "loopTask") is now solely responsible for WiFi
  // connect/reconnect - camera tasks only ever read WiFi.status(), never
  // call WiFi.begin(), so there's no race over WiFi state between tasks.
  // See WIFI_RETRY_BACKOFF_START_MS's comment for why this waits for
  // g_wifiRetryDueMs instead of retrying every single iteration.
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
      g_wifiRetryDelayMs = (g_wifiFailureStreak == 0)
          ? WIFI_RETRY_BACKOFF_START_MS
          : (g_wifiRetryDelayMs * 2UL < WIFI_RETRY_BACKOFF_MAX_MS ? g_wifiRetryDelayMs * 2UL
                                                                    : WIFI_RETRY_BACKOFF_MAX_MS);
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

  if (WiFi.status() == WL_CONNECTED && millis() - lastCommandPollMs >= TELEGRAM_COMMAND_POLL_MS) {
    lastCommandPollMs = millis();
    pollTelegramCommands(g_cameras.data(), g_cameraStates.data(), g_cameras.size());
  }

  delay(1000);
}
