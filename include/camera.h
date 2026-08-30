#pragma once
#include <Arduino.h>
#include <freertos/semphr.h> // SemaphoreHandle_t, for CameraState::stateMutex
#include "config.h"
#include "camera_store.h" // CameraConfig

// How many recent snapshots each camera keeps in memory (webserver.cpp's
// /cameras/snapshot route, webserver_cameras.cpp's Preview column) - see
// CameraState::snapshotHistory. A handful is enough for a "what led up to
// this" glance without denting PSRAM (snapshots are typically well under
// 200KB each - SNAPSHOT_MAX_BYTES_PSRAM, config.h).
static const size_t SNAPSHOT_HISTORY_SIZE = 5;

// How many recent reconnect timestamps each camera keeps - see
// CameraState::reconnectHistory.
static const size_t RECONNECT_HISTORY_SIZE = 10;

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
  // subscribed but snapshotUri is still empty (cameraTaskFn, camera.cpp) -
  // the ordinary subscription-retry loop only runs that call ONCE, from
  // the first cameraSetupSequence - so a transient failure there would
  // otherwise permanently disable photo alerts/timelapse (motion detection
  // keeps working, masking it) until a live edit or reboot.
  unsigned long lastSnapshotUriRetryMs = 0;

  // Consecutive subscription-retry failures and resulting backoff delay
  // (doubles per failure, capped; reset to 0 on success) - see cameraTaskFn.
  uint8_t retryStreak     = 0;
  unsigned long retryDelayMs = 0;

  // Consecutive PullMessages responses that were neither a recognized
  // success nor a recognized SOAP fault (cameraPullMessages, camera.cpp).
  // Reset on any recognized outcome. PULL_MESSAGES_AMBIGUOUS_LIMIT
  // consecutive occurrences forces a resubscribe instead of retrying a
  // possibly-dead pullPointUrl forever.
  uint8_t pullAmbiguousStreak = 0;
  uint32_t lastAlert      = 0;
  bool     hasAlerted     = false; // disambiguates lastAlert==0 from "alerted at boot" (see triggerMotionAlert)

  // Runtime mute via Telegram /on /off, persisted in NVS. Independent of
  // CameraConfig::enabled - camera keeps polling while muted, only the
  // Telegram send is suppressed.
  bool     alertsEnabled = true;

  // Pending auto-revert from a timed /on or /off (telegram.cpp) - 0 means
  // none scheduled. A millis() timestamp, compared the same overflow-safe
  // way as main.cpp's g_wifiRetryDueMs. Not persisted - a reboot cancels
  // any pending timer and falls back to loadAlertEnabledPref().
  unsigned long scheduledRevertDueMs = 0;
  bool          scheduledRevertToOn = false; // state to revert *to* once due

  // Updated on every non-empty SOAP response - the "is this camera alive"
  // signal for checkCameraOnlineStatus. Lock-guarded: besides
  // cameraSoapCall (this camera's own task), pushCameraSnapshot
  // (snapshot_history.cpp) also adjusts it (excluding time blocked on SD's
  // own mutex from counting as silence), reachable from loop()'s task too
  // (sendOnDemandSnapshot, via /snap).
  unsigned long lastContactMs = 0;

  // So checkCameraOnlineStatus only alerts on a state transition.
  bool     isOffline = false;

  // How many times this camera's subscription has been re-established
  // since boot (cameraTaskFn's retry loop) - counts the initial connect
  // too if it didn't succeed first try. RAM-only. Surfaced on the
  // dashboard as a "how flaky has this camera been" signal a live-status
  // snapshot alone can't show: a flapping camera (drops and reconnects,
  // subscribed again by the time anyone looks) reads identically to a
  // rock-solid one otherwise - retryStreak/pullAmbiguousStreak both reset
  // on reconnect too. Lock-guarded - written by this camera's own task,
  // read cross-task by the dashboard render.
  unsigned long totalReconnects = 0;

  // Ring of the most recent reconnect timestamps (millis()), so the
  // dashboard can show "how flaky has this camera been RECENTLY" -
  // totalReconnects alone can't distinguish a camera that reconnected
  // once, months ago, from one flapping every few minutes right now, both
  // of which read identically as "totalReconnects: 3". Same ring pattern
  // as snapshotHistory below; filtered to "within the last 24h" at render
  // time (webserver_cameras.cpp) rather than tracked as a rotating hourly
  // bucket - simpler, and a home camera's reconnect rate doesn't need
  // hour-level resolution. Lock-guarded, same as totalReconnects above.
  unsigned long reconnectHistory[RECONNECT_HISTORY_SIZE] = {0};
  size_t reconnectHistoryNext = 0;  // ring index the next reconnect writes to
  size_t reconnectHistoryCount = 0; // how many slots hold a real timestamp yet

  // Real motion detection timestamp (independent of mute/cooldown/quiet
  // hours) - the signal checkMotionWatchdog (camera.cpp) uses. Same-task-
  // only, no lock needed. Baselined to task-start time, not 0, so a camera
  // that never fires within cfg.motionWatchdogHours after boot still trips.
  unsigned long lastMotionMs = 0;
  bool     motionWatchdogTripped = false; // avoid repeat alerts until motion resumes

  // Same-task-only - last scheduled timelapse capture
  // (triggerTimelapseCapture, telegram.cpp). Baselined to task-start time,
  // same reasoning as lastMotionMs - a 0 default would fire the first
  // timelapse immediately for any task whose subscription took longer than
  // the configured interval to come up.
  unsigned long lastTimelapseMs = 0;

  // True once triggerMotionAlert (telegram.cpp) has actually sent a real
  // (non-quiet-hours) motion snapshot and is now tracking whether more
  // motion arrives before the cooldown ends - see checkPendingMotionDigest.
  // Same-task-only, no lock needed, same reasoning as lastMotionMs above.
  bool digestArmed = false;
  // How many further motion events landed while still within the cooldown
  // since that snapshot - each one gets no photo of its own (cfg's
  // alertCooldownMs is unchanged), just counted here. checkPendingMotionDigest
  // reports this as one summary text once the cooldown ends, so "did
  // motion continue after the photo, or was it a one-off" is answerable
  // without a photo per event.
  uint32_t suppressedMotionCount = 0;

  // Copied once from cfg.user/cfg.pass by resolveCameraCredentials() at
  // startup. Safe as const char*: cfg lives in main.cpp's camera vector,
  // never resized after boot, so these pointers stay valid for the process
  // lifetime.
  const char* user = nullptr;
  const char* pass = nullptr;

  // Set by requestLiveConfigReload() (webserver_cameras.cpp's save
  // handler, on loop()'s task) when this camera is edited while its task
  // is already running - nullptr means none pending. The owning task
  // claims and applies it at the top of its own loop (cameraTaskFn) and
  // frees it there; see requestLiveConfigReload's comment for why only the
  // owning task may ever write this camera's CameraConfig fields.
  CameraConfig* pendingConfig = nullptr;

  // Set by requestCameraStop() (webserver_cameras.cpp, on disable/delete of
  // an already-running camera) - the owning task claims and clears it at
  // the top of its own loop (cameraTaskFn), same polling shape as
  // pendingConfig above, and exits (vTaskDelete) instead of reconnecting.
  // See requestCameraStop's own comment for why a live task can be torn
  // down at all now (it couldn't before this).
  bool stopRequested = false;

  // A ring of the most recently sent snapshots' raw JPEG bytes (motion,
  // tamper, on-demand /snap), so the dashboard can show a timeline without
  // a fresh fetch. Owned by this struct - pushSnapshotHistory
  // (telegram.cpp) takes ownership of an already-fetched buffer, evicting
  // and freeing whichever entry it overwrites. snapshotHistoryNext is the
  // index the next push writes to (age-0/newest is always at
  // (snapshotHistoryNext - 1 + SNAPSHOT_HISTORY_SIZE) %
  // SNAPSHOT_HISTORY_SIZE); snapshotHistoryCount is how many slots hold a
  // real snapshot yet. Heap-allocated (PSRAM via heap_caps_malloc, same as
  // every other snapshot buffer - see telegram.cpp's allocateSnapshotBuffer).
  SnapshotHistoryEntry snapshotHistory[SNAPSHOT_HISTORY_SIZE];
  size_t snapshotHistoryNext = 0;
  size_t snapshotHistoryCount = 0;

  // Guards subscriptionActive, isOffline, alertsEnabled, hasAlerted,
  // lastAlert, snapshotUri, user, pass, scheduledRevertDueMs,
  // scheduledRevertToOn, pendingConfig, stopRequested, snapshotHistory
  // (+Next/Count), lastContactMs, totalReconnects, and reconnectHistory
  // (+Next/Count) - the fields both written by this
  // camera's own task and read/written from another task (webserver.cpp's
  // dashboard render and /cameras/snapshot route, main.cpp's heartbeat,
  // telegram.cpp's /on /off /snap handling, checkScheduledAlertReverts,
  // pushCameraSnapshot's lastContactMs adjustment - all on loop()'s task).
  // Every other field is touched only by the owning camera task, no lock
  // needed. Created once by cameraStateInit() before any task can see this
  // camera - see main.cpp's startMonitoring().
  SemaphoreHandle_t stateMutex = nullptr;
};

// Stages a new CameraConfig for st's camera to pick up itself, applied by
// applyPendingConfigIfAny (camera.cpp) - called by the web UI's save
// handler in two situations: editing an already-running camera (picked up
// within one ~10ms loop tick), or enabling a previously-disabled camera
// about to get a task spawned live (applied once at that task's startup
// instead). Either way, no reboot needed. Heap-allocates a copy; the
// owning task frees it once applied. A second call before the first is
// applied replaces it outright - the latest edit always wins.
//
// Deliberately NOT applied by writing straight into the live CameraConfig
// from here: st.user/st.pass hold raw `const char*` pointers into that
// same CameraConfig's user/pass Strings, and a String reassignment can
// free/reallocate its old buffer - doing that from any task other than
// the one reading through those pointers is a real use-after-free. More
// generally, CameraConfig has no locking of its own (only the fields
// CameraState::stateMutex lists do) - even an unrelated field, written
// unsynchronized while the dashboard renders that row, is a data race.
// Only the owning task ever writes its own CameraConfig; see
// cameraTaskFn's/applyPendingConfigIfAny's pendingConfig handling for the
// safe sequence (reassign cfg, then immediately re-point st.user/st.pass
// at the new buffers, before anything else can read the old ones).
void requestLiveConfigReload(CameraState& st, const CameraConfig& newConfig);

// Signals st's owning task to stop and exit (vTaskDelete) at the top of its
// next loop pass, instead of reconnecting - the live-teardown counterpart
// to requestLiveConfigReload above, for a camera that's been disabled or
// deleted via the dashboard while its task is already running. The task
// itself flips its own cfg.enabled to false right before exiting (same
// single-writer rule as requestLiveConfigReload's cfg reassignment) so
// findLiveCameraIndex-based "is this slot's task alive" checks elsewhere
// (webserver_cameras.cpp) stay accurate afterward, and so a later re-enable
// correctly spawns a fresh task rather than staging a pendingConfig nothing
// will ever read again. No SOAP Unsubscribe call - same as every other
// short-lived subscription this project creates, it's simply left to
// expire on its own (see testCameraConnection's own comment).
void requestCameraStop(CameraState& st);

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
// CameraState::lastSnapshotUriRetryMs for how a transient failure gets
// retried later instead of staying permanently broken.
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
  // live CameraConfig in place (applying a pendingConfig reload).
  CameraConfig* cfg;
  CameraState*  st;
};

// Task entry point: runs cameraSetupSequence once, then loops forever
// (retry-until-subscribed, then poll/renew). Each camera gets its own
// task, so N cameras' PullMessages long-polls overlap instead of
// serializing.
//
// Only ever touches its own CameraState, so no locking needed between
// camera tasks - CameraStateLock only matters against the web server and
// loop() task (see CameraState::stateMutex). Reads WiFi.status() and
// skips network calls while disconnected, but never calls WiFi.begin()
// itself - main.cpp's loop() remains the sole owner of WiFi connect/reconnect.
void cameraTaskFn(void* pvParameters);
