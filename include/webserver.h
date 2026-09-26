#pragma once
#include "camera.h"
#include "camera_store.h"
#include <vector>

// Starts the dashboard (PsychicHttp, port 80). liveCameras/liveStates give the
// pages live camera status and let edits apply without a reboot.
//
// Open until a login is set on the Security page (auth_store.h); after that
// every route requires it. Don't expose port 80 to the internet either way.
void startWebServer(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);
