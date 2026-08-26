#pragma once
#include "camera.h"
#include "camera_store.h"
#include <vector>

// Starts the dashboard web UI (Network / Cameras / Telegram Users /
// Firmware / Security) on port 80 (PsychicHttp, github.com/hoeken/PsychicHttp).
// Pass pointers to main.cpp's live camera config/state vectors so the Cameras
// page can show current subscription/alert/last-alert status alongside the
// persisted NVS config, and offer a Test Connection check (a live
// GetCapabilities/GetEventProperties/GetSnapshotUri probe against whatever
// is currently typed into the form) before anything is saved.
//
// No authentication until you set one on the Security page - the board
// boots wide open (trusts the LAN, same as most consumer camera web UIs)
// and nags about it with a banner on every page until a login is set, at
// which point every route (including the Firmware page's .bin upload)
// requires it - see auth_store.h and g_authMiddleware in webserver.cpp.
// Don't forward this board's port 80 out to the internet regardless.
//
// Adding, editing, or deleting a camera writes to NVS immediately, but only
// takes effect after a reboot - the live vectors passed in are read-only
// here, never mutated at runtime, to avoid the complexity/risk of spawning
// or killing a FreeRTOS task with an open TLS connection or ONVIF
// subscription mid-flight. The Firmware page is the exception - a verified
// upload reboots the board immediately (see webserver.cpp's OTA section).
void startWebServer(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);
