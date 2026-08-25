#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "camera.h"

static CameraState cameraStates[NUM_CAMERAS];

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

  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return;
  setupTime();

  for (size_t i = 0; i < NUM_CAMERAS; i++) {
    Serial.printf("\n--- Setting up camera %u/%u: %s ---\n",
                  (unsigned)(i + 1), (unsigned)NUM_CAMERAS, CAMERAS[i].name);
    if (!cameraSetupSequence(CAMERAS[i], cameraStates[i])) {
      Serial.printf("[%s] Initial setup FAILED - loop() will keep retrying.\n", CAMERAS[i].name);
    }
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi disconnected.");
    for (size_t i = 0; i < NUM_CAMERAS; i++) {
      cameraStates[i].subscriptionActive = false;
      cameraStates[i].pullPointUrl = "";
    }
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) setupTime();
    delay(1000);
    return;
  }

  // Round-robin: one camera gets serviced per loop() pass. With PT2S
  // PullMessages timeouts and N cameras, each camera's real poll cadence is
  // roughly N x a few hundred ms to ~2s - fine for a handful of cameras,
  // but see the note in the reply about parallelizing this with FreeRTOS
  // tasks if you scale up much further.
  static size_t currentCamera = 0;
  const CameraConfig& cfg = CAMERAS[currentCamera];
  CameraState& st = cameraStates[currentCamera];

  if (!st.subscriptionActive) {
    if (millis() - st.lastRetry >= RETRY_INTERVAL_MS) {
      st.lastRetry = millis();
      Serial.printf("[%s] Retrying subscription...\n", cfg.name);
      if (st.eventServiceUrl.length() == 0) {
        cameraSetupSequence(cfg, st); // full rediscovery if we never got services
      } else if (cameraGetEventServiceCapabilities(cfg, st) && cameraCreatePullPoint(cfg, st)) {
        Serial.printf("[%s] Subscription recovered.\n", cfg.name);
      }
    }
  } else {
    if (millis() - st.lastPull >= PULL_INTERVAL_MS) {
      st.lastPull = millis();
      cameraPullMessages(cfg, st);
    }
    if (millis() - st.lastRenew >= (SUBSCRIPTION_LIFETIME_MS - RENEW_MARGIN_MS)) {
      cameraRenewSubscription(cfg, st);
    }
  }

  currentCamera = (currentCamera + 1) % NUM_CAMERAS;
  delay(10);
}
