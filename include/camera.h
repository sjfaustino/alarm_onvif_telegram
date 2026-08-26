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

  // Consecutive subscription-retry failures and the resulting backoff
  // delay - see cameraTaskFn's retry logic in camera.cpp. retryDelayMs
  // starts at RETRY_INTERVAL_MS on the first failure and doubles on each
  // consecutive one after that, up to a cap; both reset to 0 the moment a
  // retry succeeds, so a camera that's merely flaky (fails once, recovers)
  // isn't held to the long delay a truly dead one has earned.
  uint8_t retryStreak     = 0;
  unsigned long retryDelayMs = 0;
  uint32_t lastAlert      = 0;
  bool     hasAlerted     = false; // lastAlert==0 is indistinguishable from "alerted at boot" - this
                                    // disambiguates so the cooldown doesn't swallow an alert that fires
                                    // within the first alertCooldownMs of boot (see triggerMotionAlert)

  // Runtime mute, toggled via Telegram (/on, /off <camera name>) and persisted
  // across reboots in NVS - see loadAlertEnabledPref/pollTelegramCommands in
  // telegram.h. Independent of CameraConfig::enabled: this camera keeps
  // polling ONVIF and its subscription stays alive while muted, only
  // triggerMotionAlert's Telegram send is suppressed.
  bool     alertsEnabled = true;

  // Updated on every non-empty SOAP response this camera sends back (see
  // cameraSoapCall in camera.cpp) - the "is this camera actually there"
  // signal for checkCameraOnlineStatus (telegram.cpp), independent of
  // whether a specific ONVIF call happens to be failing/faulting.
  unsigned long lastContactMs = 0;

  // Current known online/offline state, so checkCameraOnlineStatus only
  // alerts on a transition (going offline, or recovering) rather than
  // every time it checks.
  bool     isOffline = false;

  // Copied once from cfg.user/cfg.pass by resolveCameraCredentials() at
  // task startup. Kept as const char* (rather than reading cfg.user/pass
  // directly everywhere) so every SOAP/snapshot call in camera.cpp and
  // telegram.cpp can keep using a plain const char* the way it always has -
  // safe because cfg lives in main.cpp's camera vector, which is only ever
  // populated once at boot and never resized/reallocated afterward, so
  // these pointers (into cfg's String storage) stay valid for the process's
  // lifetime.
  const char* user = nullptr;
  const char* pass = nullptr;
};

// Copies cfg.user/cfg.pass into st.user/st.pass. Returns false (and logs)
// if either is empty, so a camera added via the web UI without credentials
// filled in fails loudly at boot instead of silently sending blank auth.
bool resolveCameraCredentials(const CameraConfig& cfg, CameraState& st);

// Runs GetCapabilities -> GetServiceCapabilities -> GetEventProperties ->
// GetProfiles/GetSnapshotUri -> CreatePullPointSubscription, in order.
// Returns false (and logs why) if any required step fails.
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

// One of these is passed (heap-allocated, owned by the task) as the pvParameters
// of a FreeRTOS task created for each enabled camera - see cameraTaskFn.
struct CameraTaskContext {
  const CameraConfig* cfg;
  CameraState*         st;
};

// Task entry point: runs cameraSetupSequence once, then loops forever doing
// what main.cpp's loop() used to do per-camera on its round-robin turn -
// retry-until-subscribed, then poll/renew on their own cadence - except now
// each camera gets its own task instead of sharing one round-robin slot, so
// N cameras' PullMessages long-polls overlap instead of serializing.
//
// Reads cfg (never mutated after boot) and only ever touches its
// own CameraState - no locking needed between camera tasks. It does read
// WiFi.status() each iteration and skips network calls while disconnected,
// but never calls WiFi.begin() itself - loop() in main.cpp remains the sole
// owner of connecting/reconnecting WiFi, so there's no race over Wi-Fi state.
void cameraTaskFn(void* pvParameters);
