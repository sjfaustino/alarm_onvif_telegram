#pragma once
#include <Arduino.h>
#include <vector>
#include "camera_store.h" // CameraConfig

// Maintenance panel content (reboot button, Internet Watchdog and Camera
// Bridge Watchdog fieldsets). Split out of webserver.cpp - see
// webserver_network.h's comment for why. The actual reboot (a delayed
// ESP.restart() from a short-lived task, so the response finishes sending
// first) stays in webserver.cpp, alongside the same pattern the Firmware
// page's OTA success path already uses.
//
// liveCameras populates the Camera Bridge Watchdog fieldset's cameraA/
// cameraB dropdowns - a free-text name field would silently go inert on a
// typo, so the form only ever offers names that actually exist right now.
String renderMaintenancePanel(const std::vector<CameraConfig>* liveCameras);
