#pragma once
#include <Arduino.h>
#include <freertos/semphr.h> // SemaphoreHandle_t, for CameraState::stateMutex
#include "config.h"
#include "camera_store.h" // CameraConfig

// How many recent snapshots (webserver.cpp's /cameras/snapshot route,
// webserver_cameras.cpp's Preview column) each camera keeps in memory -
// see CameraState::snapshotHistory's own comment. A handful is enough for
// a quick "what led up to this" glance without meaningfully denting PSRAM
// (real snapshots are typically well under 200KB each, per
// SNAPSHOT_MAX_BYTES_PSRAM's comment in config.h).
static const size_t SNAPSHOT_HISTORY_SIZE = 5;

// One cached snapshot - see CameraState::snapshotHistory.
struct SnapshotHistoryEntry {
  uint8_t* jpg = nullptr;
  size_t len = 0;
  unsigned long ms = 0; // millis() when it was captured
};

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

  // Last periodic retry of cameraFetchProfileAndSnapshotUri while
  // subscribed but snapshotUri is still empty (camera.cpp's cameraTaskFn) -
  // see that check's own comment for why this exists: the ordinary
  // subscription-retry loop only ever runs cameraFetchProfileAndSnapshotUri
  // ONCE, from the very first cameraSetupSequence call, and never again
  // once eventServiceUrl is set (every later retry only touches the event/
  // pull-point services) - a transient GetProfiles/GetSnapshotUri failure
  // at that first attempt would otherwise permanently disable photo
  // alerts/timelapse for this camera (motion detection keeps working fine,
  // masking the problem) until a live config edit or a reboot forces full
  // rediscovery.
  unsigned long lastSnapshotUriRetryMs = 0;

  // Consecutive subscription-retry failures and resulting backoff delay
  // (doubles per failure, capped; reset to 0 on success) - see cameraTaskFn.
  uint8_t retryStreak     = 0;
  unsigned long retryDelayMs = 0;

  // Consecutive PullMessages responses that were neither a recognized
  // success nor a recognized SOAP fault (genuinely malformed/unexpected
  // body - see cameraPullMessages, camera.cpp). Reset to 0 on any
  // recognized outcome, success or fault. PULL_MESSAGES_AMBIGUOUS_LIMIT
  // consecutive occurrences forces a resubscribe rather than retrying the
  // same possibly-dead pullPointUrl forever with no path back to a
  // working subscription - one occurrence alone is tolerated as noise.
  uint8_t pullAmbiguousStreak = 0;
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
  // signal for checkCameraOnlineStatus. Lock-guarded (see stateMutex's own
  // comment below) - besides cameraSoapCall (this camera's own task),
  // snapshot_history.cpp's pushCameraSnapshot also adjusts this (to
  // exclude time spent blocked on the SD subsystem's own internal mutex
  // from counting as camera silence), and that function is reachable from
  // loop()'s task too (sendOnDemandSnapshot, via /snap), not just this
  // camera's own task.
  unsigned long lastContactMs = 0;

  // So checkCameraOnlineStatus only alerts on a state transition.
  bool     isOffline = false;

  // Real motion detection timestamp (independent of mute/cooldown/quiet
  // hours) - the "is this camera still actually seeing motion" signal for
  // checkMotionWatchdog (camera.cpp). Same-task-only (written/read only by
  // this camera's own task, like lastPull/retryStreak above), no lock
  // needed. Baselined to task-start time, not 0, so a camera that never
  // fires within cfg.motionWatchdogHours after boot still trips.
  unsigned long lastMotionMs = 0;
  bool     motionWatchdogTripped = false; // avoid repeat alerts until motion resumes

  // Same-task-only (see above) - last scheduled timelapse capture
  // (triggerTimelapseCapture, telegram.cpp), independent of the alert
  // cooldown. Baselined to task-start time in cameraTaskFn (camera.cpp),
  // same reasoning as lastMotionMs above - a 0 default would make the
  // first timelapse fire immediately once subscribed, for any task whose
  // subscription took longer than the configured interval to come up.
  unsigned long lastTimelapseMs = 0;

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

  // A ring of the most recently sent snapshots' raw JPEG bytes (motion,
  // tamper, or on-demand /snap), so the dashboard can show a small
  // timeline (webserver.cpp's /cameras/snapshot route, webserver_cameras
  // .cpp's Preview column) without triggering a fresh fetch. Owned by
  // this struct - pushSnapshotHistory (telegram.cpp) takes ownership of
  // an already-fetched buffer (no extra copy), evicting and freeing
  // whichever entry it overwrites. snapshotHistoryNext is the index the
  // *next* push writes to (so age-0/newest is always at
  // (snapshotHistoryNext - 1 + SNAPSHOT_HISTORY_SIZE) %
  // SNAPSHOT_HISTORY_SIZE); snapshotHistoryCount is how many of the N
  // slots actually hold a real snapshot yet (< SNAPSHOT_HISTORY_SIZE
  // until this camera has sent that many). All zero/nullptr until the
  // first snapshot is ever sent for this camera. Heap-allocated (PSRAM
  // via heap_caps_malloc, same as every other snapshot buffer in this
  // project - see telegram.cpp's allocateSnapshotBuffer).
  SnapshotHistoryEntry snapshotHistory[SNAPSHOT_HISTORY_SIZE];
  size_t snapshotHistoryNext = 0;
  size_t snapshotHistoryCount = 0;

  // Guards subscriptionActive, isOffline, alertsEnabled, hasAlerted,
  // lastAlert, snapshotUri, user, pass, scheduledRevertDueMs,
  // scheduledRevertToOn, pendingConfig, snapshotHistory (plus its
  // Next/Count), and lastContactMs - the only fields both written by this
  // camera's own task (camera.cpp) and read/written from another task
  // (webserver.cpp's dashboard render and /cameras/snapshot route,
  // main.cpp's heartbeat, telegram.cpp's /on /off /snap command handling,
  // checkScheduledAlertReverts, and pushCameraSnapshot's lastContactMs
  // adjustment via sendOnDemandSnapshot, all on loop()'s task). Every
  // other field is touched only from the owning camera task, so it needs
  // no lock.
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

// Runs GetCapabilities -> GetProfiles/GetSnapshotUri -> GetServiceCapabilities
// -> GetEventProperties -> CreatePullPointSubscription, in order. A
// GetProfiles/GetSnapshotUri failure is deliberately non-fatal here (motion
// detection still works without a resolved snapshot URI) - see
// cameraFetchProfileAndSnapshotUri's own call site comment, and
// CameraState::lastSnapshotUriRetryMs for how a transient failure here
// gets retried later instead of staying permanently broken.
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
