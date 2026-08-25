#pragma once
#include <Arduino.h>
#include "config.h"

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
  uint32_t lastAlert      = 0;
};

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
// Reads CAMERAS[]/cfg (never mutated after boot) and only ever touches its
// own CameraState - no locking needed between camera tasks. It does read
// WiFi.status() each iteration and skips network calls while disconnected,
// but never calls WiFi.begin() itself - loop() in main.cpp remains the sole
// owner of connecting/reconnecting WiFi, so there's no race over Wi-Fi state.
void cameraTaskFn(void* pvParameters);
