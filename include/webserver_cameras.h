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
bool saveCameraSubmission(CameraConfig cam, const String& originalName, String& banner);

// Runs a live GetCapabilities -> GetServiceCapabilities/GetEventProperties
// -> GetProfiles/GetSnapshotUri -> CreatePullPointSubscription sequence
// against cfg without touching NVS - see the .cpp for the full rationale.
String testCameraConnection(CameraConfig cfg);
