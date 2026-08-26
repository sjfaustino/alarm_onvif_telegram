#pragma once
#include "camera.h"
#include "camera_store.h"
#include <vector>

// Starts the dashboard web UI (Network / Cameras / Telegram Users /
// Firmware) on port 80 (PsychicHttp, github.com/hoeken/PsychicHttp). Pass
// pointers to main.cpp's live camera config/state vectors so the Cameras
// page can show current subscription/alert/last-alert status alongside the
// persisted NVS config, and offer a Test Connection check (a live
// GetCapabilities/GetEventProperties/GetSnapshotUri probe against whatever
// is currently typed into the form) before anything is saved.
//
// No authentication - trusts the LAN, same as most consumer camera web UIs
// left open on a home network. Don't forward this board's port 80 out to
// the internet - that would also expose the Firmware page's unauthenticated
// .bin upload.
//
// Adding, editing, or deleting a camera writes to NVS immediately, but only
// takes effect after a reboot - the live vectors passed in are read-only
// here, never mutated at runtime, to avoid the complexity/risk of spawning
// or killing a FreeRTOS task with an open TLS connection or ONVIF
// subscription mid-flight. The Firmware page is the exception - a verified
// upload reboots the board immediately (see webserver.cpp's OTA section).
void startWebServer(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);
