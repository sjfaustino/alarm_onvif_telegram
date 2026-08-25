#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "camera.h"

static CameraState cameraStates[NUM_CAMERAS];

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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("MULTI-CAMERA ONVIF MOTION MONITOR");
  Serial.printf("Cameras configured: %u\n", (unsigned)NUM_CAMERAS);
  Serial.println("========================================");

  printCameraList();

  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return;
  setupTime();

  // One FreeRTOS task per enabled camera - see cameraTaskFn's comment for
  // why (overlapping each camera's PullMessages long-poll / Telegram send
  // instead of serializing them behind a shared round-robin slot). Each
  // task does its own initial cameraSetupSequence before entering its loop,
  // so setup() just needs to spawn them.
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
    xTaskCreate(cameraTaskFn, taskName, 10240, ctx, tskIDLE_PRIORITY + 1, nullptr);
  }
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
  delay(1000);
}
