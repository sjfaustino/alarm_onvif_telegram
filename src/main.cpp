#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "camera.h"
#include "telegram.h"

static CameraState cameraStates[NUM_CAMERAS];
static unsigned long lastHeartbeatMs = 0;

// Extracts "host[:port]" out of a URL like "http://192.168.1.178:8899/onvif/..."
// for the startup camera listing - config.h only stores the full service URL,
// not a separate IP field.
static String extractHost(const char* url) {
  String s(url);
  int schemeEnd = s.indexOf("://");
  int start = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int pathStart = s.indexOf('/', start);
  int end = (pathStart >= 0) ? pathStart : (int)s.length();
  return s.substring(start, end);
}

static void printCameraList() {
  Serial.println("\n--- Configured cameras ---");
  for (size_t i = 0; i < NUM_CAMERAS; i++) {
    const CameraConfig& cfg = CAMERAS[i];
    Serial.printf("  [%u] %-20s %-24s %s\n",
                  (unsigned)i, cfg.name, extractHost(cfg.deviceServiceUrl).c_str(),
                  cfg.enabled ? "enabled" : "disabled");
  }
  Serial.println("--------------------------\n");
}

// Same layout as printCameraList(), but for the Telegram boot message:
// no IP (not useful/actionable to a phone reader) and ON/OFF instead of
// enabled/disabled so it reads at a glance.
static String buildCameraListMessage() {
  String s = "--- Configured cameras ---\n";
  for (size_t i = 0; i < NUM_CAMERAS; i++) {
    const CameraConfig& cfg = CAMERAS[i];
    char line[48];
    snprintf(line, sizeof(line), "  [%u] %-20s %s\n",
              (unsigned)i, cfg.name, cfg.enabled ? "ON" : "OFF");
    s += line;
  }
  s += "--------------------------";
  return s;
}

static void connectWiFi() {
  Serial.println("\nConnecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000UL) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: WiFi connection failed.");
    return;
  }
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
}

static void setupTime() {
  Serial.println("Synchronizing UTC time...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
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
  for (size_t i = 0; i < NUM_CAMERAS; i++) {
    if (!CAMERAS[i].enabled) continue;
    msg += String(CAMERAS[i].name) + ": " +
           (cameraStates[i].subscriptionActive ? "subscribed" : "NOT subscribed") + "\n";
  }
  if (!sendTelegramMessage(msg)) {
    Serial.println("Heartbeat: Telegram send failed.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("MULTI-CAMERA ONVIF MOTION MONITOR");
  Serial.printf("Cameras configured: %u\n", (unsigned)NUM_CAMERAS);
  Serial.println("========================================");

  Serial.printf("PSRAM: %u bytes%s\n", (unsigned)ESP.getPsramSize(),
                ESP.getPsramSize() == 0 ? " (none detected)" : "");

  if (ESP.getPsramSize() > 0) {
    // Previously only telegram.cpp's explicit snapshot buffer used PSRAM -
    // every other sizeable allocation (GetProfiles/GetSnapshotUri SOAP
    // response Strings in onvif_soap.cpp/camera.cpp, which can run several
    // KB with multiple profiles) still competed with mbedTLS's own
    // internal-heap TLS buffers for internal RAM. This routes any single
    // allocation >=4KB to PSRAM automatically instead. Small/frequent
    // allocations stay on internal RAM on purpose - PSRAM access is slower
    // than internal RAM, so forcing every short-lived tag-name String
    // through it would cost more than it saves.
    heap_caps_malloc_extmem_enable(4096);
    Serial.println("PSRAM: allocations >=4KB will be routed here automatically.");
  }

  printCameraList();

  if (!telegramCAConfigured()) {
    Serial.println("WARNING: telegram_ca.h's TELEGRAM_ROOT_CA is still the placeholder - "
                    "every Telegram send will fail until you fill in the real certificate.");
  }

  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return;
  setupTime();

  // One FreeRTOS task per enabled camera, pinned to core 1 - see
  // xTaskCreatePinnedToCore's comment below and cameraTaskFn's comment in
  // camera.cpp for why (this board turned out to be a genuine dual-core
  // ESP32, so these run in true parallel, not just time-sliced). Each task
  // does its own initial cameraSetupSequence before entering its loop, so
  // setup() just needs to spawn them.
  //
  // The delay() after each spawn staggers those initial GetCapabilities
  // calls instead of firing all of them within the same handful of
  // milliseconds. Several of these cameras' embedded HTTP stacks only
  // accept 1-2 concurrent connections, so hitting all N at once caused a
  // thundering-herd of "Connection reset by peer" failures at boot even
  // though the cameras were fine individually.
  for (size_t i = 0; i < NUM_CAMERAS; i++) {
    if (!CAMERAS[i].enabled) {
      Serial.printf("[%s] Disabled - no task created.\n", CAMERAS[i].name);
      continue;
    }
    CameraTaskContext* ctx = new CameraTaskContext{&CAMERAS[i], &cameraStates[i]};
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
  for (size_t i = 0; i < NUM_CAMERAS; i++) if (CAMERAS[i].enabled) enabledCount++;

  String bootMsg = "\xF0\x9F\x93\xB7 Camera monitor online\n";
  bootMsg += String(enabledCount) + "/" + String((int)NUM_CAMERAS) + " cameras enabled\n";
  bootMsg += buildCameraListMessage();
  if (!sendTelegramMessage(bootMsg)) {
    Serial.println("Boot notice: Telegram send failed (network/token/CA issue?) - continuing anyway.");
  }

  lastHeartbeatMs = millis(); // first heartbeat fires HEARTBEAT_INTERVAL_MS from now, not immediately
}

void loop() {
  // loop() (the Arduino "loopTask") is now solely responsible for WiFi
  // connect/reconnect - camera tasks only ever read WiFi.status(), never
  // call WiFi.begin(), so there's no race over WiFi state between tasks.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi disconnected.");
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) setupTime();
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = millis();
    sendHeartbeat();
  }

  delay(1000);
}
