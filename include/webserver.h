#pragma once
#include "camera.h"
#include "camera_store.h"
#include <vector>

// Starts the camera-management web UI (add / delete / view) on port 80
// (PsychicHttp, github.com/hoeken/PsychicHttp). Pass pointers to main.cpp's
// live camera config/state vectors so the page can show current
// subscription/alert status alongside the persisted NVS config.
//
// No authentication - trusts the LAN, same as most consumer camera web UIs
// left open on a home network. Don't forward this board's port 80 out to
// the internet.
//
// Adding or deleting a camera writes to NVS immediately, but only takes
// effect after a reboot - the live vectors passed in are read-only here,
// never mutated at runtime, to avoid the complexity/risk of spawning or
// killing a FreeRTOS task with an open TLS connection or ONVIF subscription
// mid-flight.
void startWebServer(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);
