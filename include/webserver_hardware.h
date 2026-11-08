#pragma once
#include <Arduino.h>
#include <vector>
#include "camera_store.h" // CameraConfig

// One page per relay/sensor feature under the Hardware menu.

String renderInternetWatchdogPage();

// A dropdown of existing names, since a mistyped free-text name would silently
// do nothing.
String renderBridgeWatchdogPage(const std::vector<CameraConfig>* liveCameras);

String renderPowerMonitorPage();

struct CameraState;

// What each camera reports about itself over ONVIF (camera_hardware.h),
// index-aligned lists as in g_liveCameras/g_liveStates.
String renderCameraHardwarePage(const std::vector<CameraConfig>* liveCameras,
                                std::vector<CameraState>* liveStates);
