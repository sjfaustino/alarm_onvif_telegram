#pragma once
#include <Arduino.h>
#include <vector>
#include "camera.h"

// "Capabilities" page: a fleet-wide table of which detection topics each
// live camera's own ONVIF event schema actually advertises
// (CameraState::supportedEventTopics/unusedEventTopics, populated once per
// camera during cameraSetupSequence, camera.cpp) - answers "what could I
// turn on" across every camera at a glance, instead of checking Test
// Connection or the Cameras page's own per-row tooltip one camera at a
// time. Split out of webserver.cpp - see webserver_network.h's comment
// for why.
//
// A camera that's disabled, or was added after this board's current boot
// (no live CameraState slot yet), has never run cameraSetupSequence this
// session - shown with a note instead of a blank row, rather than being
// silently omitted, so its absence isn't mistaken for "this camera has no
// detection capability at all".
String renderCapabilitiesPanel(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates);
