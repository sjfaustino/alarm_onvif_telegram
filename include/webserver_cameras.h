#pragma once
#include <Arduino.h>
#include <PsychicHttp.h>
#include <vector>
#include "camera.h"
#include "camera_store.h"
#include "onvif_discovery.h" // DiscoveredCamera

// Cameras panel: live status table, Add/Edit form, Test Connection. Split
// out of webserver.cpp - see webserver_network.h's comment for why.

// prefill/isEdit repopulate the form after an edit link, a failed save, or
// a Test Connection round trip - null prefill is the blank "Add" state.
// liveCameras/liveStates are startWebServer()'s live vectors, read only to
// show current subscription/alert status alongside the persisted config.
String renderCamerasPanel(const CameraConfig* prefill, bool isEdit,
                           std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);

// Reads the Add/Edit camera form into a CameraConfig - used both to save
// (saveCameraSubmission) and to test a connection without saving.
CameraConfig parseCameraForm(PsychicRequest* request);

// originalName is "" for a new camera, non-empty for an edit (cam.name may
// differ - a rename). A blank password on an edit keeps the current one.
//
// liveCameras/liveStates (startWebServer()'s live vectors, same ones
// renderCamerasPanel reads) let a save apply immediately to a camera
// that's already running, instead of always requiring a reboot:
//   - editing a camera whose task is already running (still enabled
//     before and after) stages the new config via requestLiveConfigReload
//     (camera.h) - the owning task picks it up and reconnects within
//     ~10ms, no reboot needed.
//   - flipping a previously-disabled camera to enabled spawns its task
//     live (camera_tasks.h) - same as it would get at the next boot.
//   - disabling a camera whose task is already running stops it live too
//     (requestCameraStop, camera.h) - the task exits on its own next loop
//     pass, no reboot needed.
//   - a brand new camera (not yet in liveCameras - added after this
//     board's current boot) still needs a reboot: liveCameras/liveStates
//     are sized once at boot and never grow, so there's no slot to spawn
//     a task into yet. applyNote explains which case just happened, in
//     plain English, for the caller to show as a banner after redirecting
//     back to /cameras - "" if nothing live happened (the ordinary
//     reboot-required case, unchanged from before this).
bool saveCameraSubmission(CameraConfig cam, const String& originalName, String& banner, String& applyNote,
                           std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);

// Stops name's live task (requestCameraStop) if it's currently running,
// returning whether it was. Call this from the /delete route BEFORE (or
// after - order doesn't matter, they touch different stores) removing the
// camera from NVS, so a deleted camera's task doesn't keep monitoring and
// alerting on a camera the dashboard no longer lists.
bool stopLiveCameraIfRunning(const String& name, std::vector<CameraConfig>* liveCameras,
                              std::vector<CameraState>* liveStates);

// Reads the same quietHoursEnabled/quietStart/quietEnd fields the
// per-camera Add/Edit form uses and overwrites EVERY camera's quiet hours
// with them at once (wholesale, via replaceAllCameras - see the .cpp for
// why this doesn't skip disabled cameras). Live-reloads every already-
// running enabled camera the same way a single-camera edit would; returns
// a result string for the caller to show as a banner. Call this from the
// /cameras/quiet-hours-all route handler.
String applyQuietHoursToAllCameras(PsychicRequest* request, std::vector<CameraConfig>* liveCameras,
                                    std::vector<CameraState>* liveStates);

// Runs a live GetCapabilities -> GetServiceCapabilities/GetEventProperties
// -> GetProfiles/GetSnapshotUri -> CreatePullPointSubscription sequence
// against cfg without touching NVS - see the .cpp for the full rationale.
String testCameraConnection(CameraConfig cfg);

// One camera's result from testAllCameraConnections below - a condensed
// version of what testCameraConnection's own prose paragraph says, sized
// for a one-row-per-camera summary table instead of a full paragraph per
// camera. Deliberately does NOT include a CreatePullPointSubscription
// check the way testCameraConnection's single-camera test does - see
// testAllCameraConnections' own comment for why: an already-monitored
// camera (the common case "test all" runs against) already has a real,
// live subscription from its own running task, and creating a second one
// per click risks disrupting it on a camera firmware that only supports
// one active subscription at a time. reachable/eventServiceOk are a
// strong enough "did my network change break this camera" signal without
// that risk.
struct CameraTestResult {
  String name;
  bool skipped = false;        // true only for a disabled camera - nothing was actually tested
  bool reachable = false;      // cameraDiscoverServices succeeded
  bool eventServiceOk = false; // GetServiceCapabilities/GetEventProperties - only meaningful if reachable
  String detail;               // short human reason for the first failure, "" if fully OK
};

// Tests every ENABLED camera currently in NVS (not whatever's typed into
// the Add/Edit form) - a disabled camera is reported as skipped, not
// probed. This is the actual (slow) work - see startTestAllCamerasAsync
// below for why nothing calls this directly from a request handler.
std::vector<CameraTestResult> testAllCameraConnections();

// Renders testAllCameraConnections' results as a summary table, for use as
// a renderShell banner (raw HTML, like testCameraConnection's own string).
String renderCameraTestAllResults(const std::vector<CameraTestResult>& results);

// Starts testAllCameraConnections() on a background FreeRTOS task instead
// of running it on the calling task - see testAllCameraConnections' own
// comment for why a synchronous bulk test would block the whole
// dashboard, not just the requester, for potentially minutes. A no-op
// (doesn't start a second overlapping run) if a test is already in
// progress. Call this from the /cameras/test-all route handler.
void startTestAllCamerasAsync();

// Renders the current bulk-test status: "a test is running" while one is
// in progress, the last completed run's results table once one exists, or
// "" if no test has ever run this boot. Safe to call from any task
// (internally locked) - renderCamerasPanel calls this itself, so it shows
// up on a normal page load too, not just right after clicking the button.
String renderTestAllStatus();

// ============================================================
// Network camera discovery (WS-Discovery) - like an NVR's own "search the
// network" button. onvif_discovery.h (lib/) has the pure Probe-message-
// building/ProbeMatch-parsing logic; these two functions are the ESP32-
// specific UDP send/receive and background-task glue around it, same
// split as testAllCameraConnections above.
// ============================================================

// Starts a WS-Discovery probe on a background FreeRTOS task instead of the
// calling (PsychicHttp) task - same reasoning as startTestAllCamerasAsync
// above: the listen window is a few seconds by design (has to give slower
// cameras time to answer a multicast probe), which would otherwise block
// the whole dashboard for that long. A no-op if a search is already in
// progress. Call this from the /cameras/discover route handler.
void startCameraDiscoveryAsync();

// Renders the current discovery status: "a search is running" while one is
// in progress, the last completed run's results (a table with an Add link
// per discovered camera, prefilling the Add-camera form below with its
// address and best-effort name - WS-Discovery never carries credentials,
// so the user still types a username/password by hand) once one exists,
// or "" if no search has run yet this boot. Safe to call from any task
// (internally locked) - renderCamerasPanel calls this itself.
String renderCameraDiscoveryStatus();
