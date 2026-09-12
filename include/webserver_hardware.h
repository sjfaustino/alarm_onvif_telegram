#pragma once
#include <Arduino.h>
#include <vector>
#include "camera_store.h" // CameraConfig

// The three relay/sensor peripherals this project supports - Internet
// Watchdog, Camera Bridge Watchdog, 220V Power Monitor - each get their
// own page under the sidebar's "Hardware" submenu (see webserver.cpp's
// renderShell), instead of being stacked as fieldsets on the Maintenance
// page. Each function here renders one full page's content (an <h1> plus
// its one fieldset), the same shape renderMaintenancePanel/renderNetworkPanel/
// etc. already use elsewhere.

String renderInternetWatchdogPage();

// liveCameras populates the cameraA/cameraB dropdowns - a free-text name
// field would silently go inert on a typo, so the form only ever offers
// names that actually exist right now.
String renderBridgeWatchdogPage(const std::vector<CameraConfig>* liveCameras);

String renderPowerMonitorPage();
