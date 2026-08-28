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

  // Pending auto-revert from a timed /on or /off (telegram.cpp's
  // handleTelegramCommand/checkScheduledAlertReverts) - 0 means none
  // scheduled. A millis() timestamp, not wall-clock - compared the same
  // overflow-safe way as main.cpp's g_wifiRetryDueMs ((long)(millis() -
  // due) >= 0), not a plain >=. Deliberately not persisted to NVS: a
  // reboot cancels any pending timer and falls back to whatever
  // loadAlertEnabledPref() last had saved, same as every other purely
  // in-RAM per-boot field here.
  unsigned long scheduledRevertDueMs = 0;
  bool          scheduledRevertToOn = false; // state to revert *to* once due

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

  // Set by requestLiveConfigReload() (called from webserver_cameras.cpp's
  // save handler, on loop()'s task) when this camera is edited via the
  // dashboard while its task is already running - nullptr means none
  // pending. The owning task claims and applies it at the top of its own
  // loop (cameraTaskFn) and frees it there; see requestLiveConfigReload's
  // own comment for why only the owning task may ever write this camera's
  // CameraConfig fields, never the task that stages a reload.
  CameraConfig* pendingConfig = nullptr;

  // Guards subscriptionActive, isOffline, alertsEnabled, hasAlerted,
  // lastAlert, snapshotUri, user, pass, scheduledRevertDueMs,
  // scheduledRevertToOn, and pendingConfig - the only fields both written
  // by this camera's own task (camera.cpp) and read from another task
  // (webserver.cpp's dashboard render, main.cpp's heartbeat, telegram.cpp's
  // /on /off /snap command handling and checkScheduledAlertReverts, all on
  // loop()'s task). Every other field is touched only from the owning
  // camera task, so it needs no lock.
  // Created once by cameraStateInit() before any task can see this camera -
  // see main.cpp's startMonitoring().
  SemaphoreHandle_t stateMutex = nullptr;
};

// Stages a new CameraConfig for st's camera to pick up itself, applied by
// applyPendingConfigIfAny (camera.cpp) - called by the web UI's save
// handler (webserver_cameras.cpp) in two situations: editing a camera
// whose task is already running (picked up within one ~10ms loop tick -
// cameraTaskFn checks at the top of every pass), or enabling a
// previously-disabled camera that's about to get a task spawned for it
// live, where it's applied once at that task's own startup instead.
// Either way, no reboot needed. Heap-allocates a copy; the owning task
// frees it once applied. A second call before the first is applied
// replaces it outright (the latest edit always wins) rather than queuing
// both.
//
// Deliberately NOT applied by writing straight into the live CameraConfig
// from here (or from webserver_cameras.cpp/main.cpp for the live-spawn
// case): st.user/st.pass hold raw `const char*` pointers into that same
// CameraConfig's user/pass Strings (see their own comment above), and a
// String reassignment can free/reallocate its old buffer - doing that
// from any task other than the one that might read through those pointers
// (mid-SOAP-call, or the moment a task starts using them) is a real use-
// after-free. More generally, CameraConfig itself has no locking of its
// own at all (only the fields CameraState::stateMutex's own comment lists
// do) - even a field unrelated to user/pass, written unsynchronized from
// a different task while e.g. the dashboard renders that same camera's
// row, is a data race the C++ memory model gives no guarantees about.
// Only the owning task ever writes its own CameraConfig's fields; see
// cameraTaskFn's/applyPendingConfigIfAny's handling of pendingConfig for
// the safe sequence (reassign cfg, then immediately re-point st.user/
// st.pass at the new buffers, unconditionally, before anything else can
// read the old ones).
void requestLiveConfigReload(CameraState& st, const CameraConfig& newConfig);

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
  // Non-const, unlike every other function in this file's CameraConfig
  // parameter: cameraTaskFn is the one place that legitimately mutates a
  // live CameraConfig in place (applying a pendingConfig reload) - see
  // requestLiveConfigReload's comment. Every SOAP/discovery function still
  // takes `const CameraConfig&`, which a non-const object binds to fine.
  CameraConfig* cfg;
  CameraState*  st;
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
