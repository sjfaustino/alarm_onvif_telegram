#pragma once
#include <Arduino.h>
#include <freertos/semphr.h> // SemaphoreHandle_t, for CameraState::stateMutex
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

  // Guards subscriptionActive, isOffline, alertsEnabled, hasAlerted,
  // lastAlert, snapshotUri, user, and pass - the only fields both written
  // by this camera's own task (camera.cpp) and read from another task
  // (webserver.cpp's dashboard render, main.cpp's heartbeat, telegram.cpp's
  // /on /off /snap command handling on loop()'s task). Every other field
  // is touched only from the owning camera task, so it needs no lock.
  // Created once by cameraStateInit() before any task can see this camera -
  // see main.cpp's startMonitoring().
  SemaphoreHandle_t stateMutex = nullptr;
};

// Creates st.stateMutex. Call once per camera before spawning its task or
// starting the web server - see main.cpp's startMonitoring().
void cameraStateInit(CameraState& st);

// RAII lock for st.stateMutex - take it around any read or write of the
// cross-task fields listed in CameraState's stateMutex comment.
class CameraStateLock {
 public:
  explicit CameraStateLock(CameraState& state) : st_(state) {
    if (st_.stateMutex) xSemaphoreTake(st_.stateMutex, portMAX_DELAY);
  }
  ~CameraStateLock() {
    if (st_.stateMutex) xSemaphoreGive(st_.stateMutex);
  }
  CameraStateLock(const CameraStateLock&) = delete;
  CameraStateLock& operator=(const CameraStateLock&) = delete;

 private:
  CameraState& st_;
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
// Only ever touches its own CameraState, so no locking needed between
// camera tasks - CameraStateLock only matters against the web server and
// loop() task, which read/write a handful of its fields concurrently (see
// CameraState::stateMutex). Reads WiFi.status() and skips network calls
// while disconnected,
// but never calls WiFi.begin() itself - main.cpp's loop() remains the sole
// owner of WiFi connect/reconnect.
void cameraTaskFn(void* pvParameters);
