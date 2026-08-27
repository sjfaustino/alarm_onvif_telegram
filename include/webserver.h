#pragma once
#include "camera.h"
#include "camera_store.h"
#include <vector>

// Starts the dashboard web UI (Network / Cameras / Telegram Users /
// Firmware / Security) on port 80 (PsychicHttp). liveCameras/liveStates let
// the Cameras page show live subscription/alert status alongside the
// persisted NVS config, and run a Test Connection probe before saving.
//
// No authentication until set on the Security page - boots wide open,
// nags with a banner until a login is set, at which point every route
// (including firmware upload) requires it - see auth_store.h. Don't
// forward port 80 to the internet regardless.
//
// Camera add/edit/delete writes to NVS immediately but only takes effect
// after a reboot - the live vectors are read-only here, never mutated at
// runtime, to avoid killing a camera task mid-subscription. The Firmware
// page is the exception - a verified upload reboots immediately.
void startWebServer(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);
