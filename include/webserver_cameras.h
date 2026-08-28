#pragma once
#include <Arduino.h>
#include <PsychicHttp.h>
#include <vector>
#include "camera.h"
#include "camera_store.h"

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
//   - a brand new camera (not yet in liveCameras - added after this
//     board's current boot) and disabling/deleting a camera whose task is
//     already running both still need a reboot: liveCameras/liveStates
//     are sized once at boot and never grow, and there's no live task-
//     teardown path yet. applyNote explains which case just happened, in
//     plain English, for the caller to show as a banner after redirecting
//     back to /cameras - "" if nothing live happened (the ordinary
//     reboot-required case, unchanged from before this).
bool saveCameraSubmission(CameraConfig cam, const String& originalName, String& banner, String& applyNote,
                           std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);

// Runs a live GetCapabilities -> GetServiceCapabilities/GetEventProperties
// -> GetProfiles/GetSnapshotUri -> CreatePullPointSubscription sequence
// against cfg without touching NVS - see the .cpp for the full rationale.
String testCameraConnection(CameraConfig cfg);
