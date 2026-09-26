#pragma once
#include <Arduino.h>
#include <freertos/semphr.h> // SemaphoreHandle_t, for CameraState::stateMutex
#include "config.h"
#include "camera_store.h" // CameraConfig
#include "snapshot_source.h" // SnapshotSource

// Recent snapshots kept in RAM per camera for the dashboard preview.
static const size_t SNAPSHOT_HISTORY_SIZE = 5;

// Size of CameraState's timestamp rings (reconnects, offline transitions).
static const size_t EVENT_HISTORY_RING_SIZE = 10;

struct SnapshotHistoryEntry {
  uint8_t* jpg = nullptr;
  size_t len = 0;
  unsigned long ms = 0; // millis() when it was captured
  SnapshotSource source = SnapshotSource::Motion; // what triggered this capture - see snapshot_source.h
};

struct CameraState {
  String   eventServiceUrl;
  String   mediaServiceUrl;
  String   pullPointUrl;
  String   snapshotUri;
  // RTSP live-view URI (GetStreamUri). Best-effort; empty when it fails or the
  // camera uses snapshotUriOverride.
  String   streamUri;
  // MJPEG-over-HTTP URI from a JPEG profile, for the dashboard's inline <img>
  // preview. Usually empty: most cameras have no JPEG profile or no HTTP
  // transport.
  String   mjpegUri;
  // Known detection topics (PeopleDetect, MotionAlarm, ...) this camera's
  // GetEventProperties mentions, comma-joined. "" until that call succeeds.
  String   supportedEventTopics;
  // Advertised topics that aren't known keywords - candidates for future
  // support.
  String   unusedEventTopics;
  String   profileToken;
  bool     subscriptionActive = false;
  unsigned long lastPull  = 0;
  unsigned long lastRenew = 0;
  unsigned long lastRetry = 0;

  // Last retry of the profile/snapshot URI fetch while subscribed. Without it
  // a transient failure at setup would disable photos until a reboot.
  unsigned long lastSnapshotUriRetryMs = 0;

  // Consecutive subscription failures and the resulting backoff delay.
  uint8_t retryStreak     = 0;
  unsigned long retryDelayMs = 0;

  // Consecutive unrecognized PullMessages responses; at
  // PULL_MESSAGES_AMBIGUOUS_LIMIT the task resubscribes.
  uint8_t pullAmbiguousStreak = 0;
  uint32_t lastAlert      = 0;
  bool     hasAlerted     = false; // disambiguates lastAlert==0 from "alerted at boot" (see triggerMotionAlert)

  // Telegram /on /off mute, persisted. Independent of CameraConfig::enabled:
  // polling continues while muted.
  bool     alertsEnabled = true;

  // millis() due time of a timed /on or /off revert; 0 = none. Not persisted.
  unsigned long scheduledRevertDueMs = 0;
  bool          scheduledRevertToOn = false; // state to revert *to* once due

  // Last non-empty SOAP response - the liveness signal for
  // checkCameraOnlineStatus. Also adjusted by pushCameraSnapshot (so SD waits
  // don't count as silence), hence locked.
  unsigned long lastContactMs = 0;

  bool     isOffline = false;

  // Subscriptions (re)established since boot, shown on the dashboard.
  unsigned long totalReconnects = 0;

  // Recent reconnect timestamps; the dashboard counts those within 24h.
  unsigned long reconnectHistory[EVENT_HISTORY_RING_SIZE] = {0};
  size_t reconnectHistoryNext = 0;  // ring index the next reconnect writes to
  size_t reconnectHistoryCount = 0; // how many slots hold a real timestamp yet

  // Recent online->offline transition timestamps, same use as
  // reconnectHistory.
  unsigned long offlineHistory[EVENT_HISTORY_RING_SIZE] = {0};
  size_t offlineHistoryNext = 0;
  size_t offlineHistoryCount = 0;

  // Recent motion-to-first-photo latencies (ms), successful fetches only. The
  // Cameras page rolls these up to guide pollIntervalMs.
  unsigned long motionLatencyHistory[EVENT_HISTORY_RING_SIZE] = {0};
  size_t motionLatencyHistoryNext = 0;
  size_t motionLatencyHistoryCount = 0;

  // Last real motion (ignoring mute/cooldown/quiet hours), for the motion
  // watchdog. Baselined to task start so a camera that never fires still trips
  // it.
  unsigned long lastMotionMs = 0;
  bool     motionWatchdogTripped = false; // avoid repeat alerts until motion resumes

  // Baselined to task start so the first timelapse doesn't fire immediately.
  unsigned long lastTimelapseMs = 0;

  // Refreshed every loop pass while subscribed. Catches a camera that keeps
  // answering (lastContactMs stays fresh) but can never hold a subscription.
  unsigned long lastSubscribedMs = 0;
  bool subscriptionLostAlerted = false; // avoid repeat alerts until subscribed again - see checkMotionWatchdog's motionWatchdogTripped for the same pattern

  // Set after a real motion photo is sent; motion during the cooldown is then
  // counted and summarised by checkPendingMotionDigest.
  bool digestArmed = false;
  uint32_t suppressedMotionCount = 0;
  // Lets the digest report how long motion actually lasted, not how long the
  // cooldown was.
  unsigned long lastSuppressedMotionMs = 0;

  // Detections since the last daily digest, which reads and resets them from
  // loop()'s task.
  uint32_t digestPersonCount = 0;
  uint32_t digestVehicleCount = 0;
  uint32_t digestPetCount = 0;
  uint32_t digestMotionCount = 0; // generic/plain motion - no finer classification available

  // Point into cfg.user/cfg.pass (set by resolveCameraCredentials). Valid for
  // the process lifetime: g_cameras never reallocates (see MAX_CAMERAS).
  const char* user = nullptr;
  const char* pass = nullptr;

  // Config edit staged by requestLiveConfigReload; the owning task applies and
  // frees it.
  CameraConfig* pendingConfig = nullptr;

  // Set by requestCameraStop; the owning task exits at the top of its loop.
  bool stopRequested = false;

  // True while fetchOneSnapshot has a GET in flight to this camera. The camera
  // task skips its own SOAP calls meanwhile - some cameras only accept 1-2
  // connections.
  bool snapshotInFlight = false;

  // Ring of recently sent snapshots (PSRAM buffers owned here).
  // snapshotHistoryNext is the next write slot; Count is how many are filled.
  SnapshotHistoryEntry snapshotHistory[SNAPSHOT_HISTORY_SIZE];
  size_t snapshotHistoryNext = 0;
  size_t snapshotHistoryCount = 0;

  // Guards the fields other tasks (web server, loop()) also touch:
  // subscriptionActive, isOffline, alertsEnabled, hasAlerted, lastAlert,
  // snapshotUri, streamUri, mjpegUri, supportedEventTopics, unusedEventTopics,
  // the digest*Count fields, user, pass, scheduledRevert*, pendingConfig,
  // stopRequested, snapshotInFlight, lastContactMs, totalReconnects, and the
  // snapshot/reconnect/offline/motionLatency rings. Everything else belongs to
  // the owning camera task alone.
  SemaphoreHandle_t stateMutex = nullptr;
};

// Stages newConfig for st's task to apply itself - either live on its next
// loop pass, or at startup of a task about to be spawned. Latest call wins.
//
// Only the owning task may write its CameraConfig: st.user/st.pass point into
// its Strings, and CameraConfig has no lock of its own.
void requestLiveConfigReload(CameraState& st, const CameraConfig& newConfig);

// Tells st's task to exit instead of reconnecting (camera disabled or deleted
// live). The task clears its own cfg.enabled first, so a later re-enable
// spawns a fresh task. The subscription is left to expire.
void requestCameraStop(CameraState& st);

// Creates st.stateMutex. Call before the task or the web server can see st.
void cameraStateInit(CameraState& st);

// RAII lock for st.stateMutex.
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

bool resolveCameraCredentials(const CameraConfig& cfg, CameraState& st);

// GetCapabilities -> profiles/snapshot URI -> service capabilities -> event
// properties -> CreatePullPointSubscription. A snapshot-URI failure is
// non-fatal; it's retried later (lastSnapshotUriRetryMs).
bool cameraSetupSequence(const CameraConfig& cfg, CameraState& st);

// Individual steps, so a task can resubscribe without redoing discovery.
bool cameraDiscoverServices(const CameraConfig& cfg, CameraState& st);
bool cameraGetEventServiceCapabilities(const CameraConfig& cfg, CameraState& st);
// outTopics/outUnusedTopics receive the topic scans on success, for callers
// using a throwaway CameraState (Test Connection).
bool cameraGetEventProperties(const CameraConfig& cfg, CameraState& st, String* outTopics = nullptr,
                               String* outUnusedTopics = nullptr);
bool cameraFetchProfileAndSnapshotUri(const CameraConfig& cfg, CameraState& st);
bool cameraCreatePullPoint(const CameraConfig& cfg, CameraState& st);
bool cameraPullMessages(const CameraConfig& cfg, CameraState& st);
bool cameraRenewSubscription(const CameraConfig& cfg, CameraState& st);

// Per-camera task parameter, owned by the task.
struct CameraTaskContext {
  // Non-const: the task applies pendingConfig reloads in place.
  CameraConfig* cfg;
  CameraState*  st;
};

// Task entry point: setup, then retry-until-subscribed and poll/renew forever.
// Never calls WiFi.begin() - loop() owns reconnecting.
void cameraTaskFn(void* pvParameters);
