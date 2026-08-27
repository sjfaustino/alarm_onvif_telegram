#pragma once
#include <Arduino.h>
#include "config.h"
#include "camera_store.h" // CameraConfig

struct CameraState {
  String   eventServiceUrl;
  String   mediaServiceUrl;
  String   pullPointUrl;
  String   snapshotUri;
  String   profileToken;
  bool     subscriptionActive = false;
  unsigned long lastPull  = 0;
  unsigned long lastRenew = 0;
  unsigned long lastRetry = 0;

  // Consecutive subscription-retry failures and resulting backoff delay
  // (doubles per failure, capped; reset to 0 on success) - see cameraTaskFn.
  uint8_t retryStreak     = 0;
  unsigned long retryDelayMs = 0;
  uint32_t lastAlert      = 0;
  bool     hasAlerted     = false; // disambiguates lastAlert==0 from "alerted at boot" (see triggerMotionAlert)

  // Runtime mute via Telegram /on /off, persisted in NVS. Independent of
  // CameraConfig::enabled - camera keeps polling while muted, only the
  // Telegram send is suppressed.
  bool     alertsEnabled = true;

  // Updated on every non-empty SOAP response - the "is this camera alive"
  // signal for checkCameraOnlineStatus.
  unsigned long lastContactMs = 0;

  // So checkCameraOnlineStatus only alerts on a state transition.
  bool     isOffline = false;

  // Copied once from cfg.user/cfg.pass by resolveCameraCredentials() at
  // startup. Safe as const char*: cfg lives in main.cpp's camera vector,
  // never resized after boot, so these pointers stay valid for the process
  // lifetime.
  const char* user = nullptr;
  const char* pass = nullptr;
};

// Copies cfg.user/cfg.pass into st.user/st.pass. Returns false (and logs)
// if either is empty.
bool resolveCameraCredentials(const CameraConfig& cfg, CameraState& st);

// Runs GetCapabilities -> GetServiceCapabilities -> GetEventProperties ->
// GetProfiles/GetSnapshotUri -> CreatePullPointSubscription, in order.
bool cameraSetupSequence(const CameraConfig& cfg, CameraState& st);

// Individual steps, exposed separately so main.cpp can retry just the
// subscription without re-doing capability discovery every time.
bool cameraDiscoverServices(const CameraConfig& cfg, CameraState& st);
bool cameraGetEventServiceCapabilities(const CameraConfig& cfg, CameraState& st);
bool cameraGetEventProperties(const CameraConfig& cfg, CameraState& st);
bool cameraFetchProfileAndSnapshotUri(const CameraConfig& cfg, CameraState& st);
bool cameraCreatePullPoint(const CameraConfig& cfg, CameraState& st);
bool cameraPullMessages(const CameraConfig& cfg, CameraState& st);
bool cameraRenewSubscription(const CameraConfig& cfg, CameraState& st);

// Heap-allocated, owned by the task - passed as pvParameters to each
// per-camera FreeRTOS task.
struct CameraTaskContext {
  const CameraConfig* cfg;
  CameraState*         st;
};

// Task entry point: runs cameraSetupSequence once, then loops forever
// (retry-until-subscribed, then poll/renew on its own cadence). Each camera
// gets its own task, so N cameras' PullMessages long-polls overlap instead
// of serializing.
//
// Only touches its own CameraState, so no locking needed between camera
// tasks. Reads WiFi.status() and skips network calls while disconnected,
// but never calls WiFi.begin() itself - main.cpp's loop() remains the sole
// owner of WiFi connect/reconnect.
void cameraTaskFn(void* pvParameters);
