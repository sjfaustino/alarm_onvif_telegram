#pragma once
#include <Arduino.h>
#include <PsychicHttp.h>
#include <vector>
#include "camera.h"
#include "camera_store.h"
#include "onvif_discovery.h" // DiscoveredCamera
#include "background_job.h" // BackgroundJobStartOutcome
#include "telegram_i18n.h" // MotionDetectionKind - startTestAlertAsync

// Cameras panel: status table, Add/Edit form, bulk actions, background tests.

// prefill/isEdit repopulate the form (null prefill = blank Add form).
// liveCameras/liveStates supply live status next to the stored config.
String renderCamerasPanel(const CameraConfig* prefill, bool isEdit,
                           std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);

// Add/Edit form -> CameraConfig, for both saving and Test Connection.
CameraConfig parseCameraForm(PsychicRequest* request);

// originalName is "" for a new camera (cam.name differs on a rename). A blank
// password on edit keeps the current one. Applies live where possible: a
// running camera reloads its config, a newly enabled one gets a task, a newly
// disabled one stops, and a new camera is staged for loop() to add
// (stagePendingNewCamera). applyNote says which happened ("" = reboot needed).
bool saveCameraSubmission(CameraConfig cam, const String& originalName, String& banner, String& applyNote,
                           std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);

// Stops name's running task, if any; call from /delete so a deleted camera
// stops alerting.
bool stopLiveCameraIfRunning(const String& name, std::vector<CameraConfig>* liveCameras,
                              std::vector<CameraState>* liveStates);

// Overwrites every camera's quiet hours from the form and live-reloads the
// running ones. Returns banner text.
String applyQuietHoursToAllCameras(PsychicRequest* request, std::vector<CameraConfig>* liveCameras,
                                    std::vector<CameraState>* liveStates);

// Sets personAlertsEnabled on every camera (live-reloading running ones).
// Person, vehicle and pet each get their own button: a combined form once
// silently re-enabled a camera's deliberately-disabled vehicle alerts.
String applyPersonAlertsToAllCameras(PsychicRequest* request, std::vector<CameraConfig>* liveCameras,
                                      std::vector<CameraState>* liveStates);

String applyVehicleAlertsToAllCameras(PsychicRequest* request, std::vector<CameraConfig>* liveCameras,
                                       std::vector<CameraState>* liveStates);

String applyPetAlertsToAllCameras(PsychicRequest* request, std::vector<CameraConfig>* liveCameras,
                                   std::vector<CameraState>* liveStates);

// Full live connection test against cfg without saving: capabilities, events,
// profiles/snapshot URI, a test subscription. Up to ~60s, so it's always run
// via startTestConnectionAsync.
String testCameraConnection(CameraConfig cfg);

// ============================================================
// Background jobs. PsychicHttp serves one request at a time, so slow work runs
// on its own task and the page polls for the result.
// ============================================================

// Starts testCameraConnection on a task (cfg is copied). Returns whether it
// started or one was already running.
BackgroundJobStartOutcome startTestConnectionAsync(const CameraConfig& cfg);

// Test Connection status HTML: running, last result, or "".
String renderTestConnectionStatus();

// One camera's Test-all result. No test subscription, unlike the single test:
// the camera's own task already holds one, and some firmware allows only one.
struct CameraTestResult {
  String name;
  bool skipped = false;        // true only for a disabled camera - nothing was actually tested
  bool reachable = false;      // cameraDiscoverServices succeeded
  bool eventServiceOk = false; // GetServiceCapabilities/GetEventProperties - only meaningful if reachable
  String detail;               // short human reason for the first failure, "" if fully OK
};

// Tests every enabled camera in NVS; disabled ones are reported as skipped.
std::vector<CameraTestResult> testAllCameraConnections();

String renderCameraTestAllResults(const std::vector<CameraTestResult>& results);

// Starts testAllCameraConnections on a task. Also deferred (AlreadyRunning)
// while a Telegram send is in flight, to avoid overlapping its memory use.
BackgroundJobStartOutcome startTestAllCamerasAsync();

String renderTestAllStatus();

// ============================================================
// Network camera discovery (WS-Discovery). lib/onvif_discovery holds the pure
// probe/parse logic; this is the UDP and task glue.
// ============================================================

// Starts a probe on a task (the listen window is seconds long). Deferred while
// a Telegram send is in flight - overlapping the two once nearly exhausted the
// heap.
BackgroundJobStartOutcome startCameraDiscoveryAsync();

// Discovery results with an Add link per camera (prefilling address and name;
// credentials are never discovered).
String renderCameraDiscoveryStatus();

// ============================================================
// Send Test Alert (per camera): exercises fetch, recipient filtering and
// delivery.
// ============================================================

// Starts sendTestAlert on a task. cfg is copied; st must be the live
// CameraState (liveStates is never reallocated) since the test uses its
// resolved snapshot URI and credentials.
BackgroundJobStartOutcome startTestAlertAsync(const CameraConfig& cfg, CameraState& st,
                                               MotionDetectionKind kind = MotionDetectionKind::Generic,
                                               bool isPetEvent = false);

String renderTestAlertStatus();

// True while any background camera job runs, so the page auto-refreshes.
bool cameraJobsInProgress();
