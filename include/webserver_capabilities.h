#pragma once
#include <Arduino.h>
#include <vector>
#include "camera.h"

// Capabilities page: which detection topics each camera advertises, across the
// fleet. Cameras that haven't run setup this boot (disabled, or no task yet)
// get a note rather than being omitted.
String renderCapabilitiesPanel(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);
