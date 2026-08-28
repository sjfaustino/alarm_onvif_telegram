#pragma once
#include <Arduino.h>
#include <vector>
#include "camera.h"

// Gallery panel content: browse a camera's stored snapshot history beyond
// the Cameras page's 5-entry Preview strip. cameraFilter == "" renders the
// camera picker (every camera with any history); a non-empty value
// renders that camera's thumbnail grid. Split out of webserver.cpp - see
// webserver_network.h's comment for why.
//
// Deliberately reuses the existing /cameras/snapshot?name=&age= route
// (webserver.cpp) for every thumbnail/full-size image instead of adding a
// new SD read path - that route already dispatches through
// readCameraSnapshot (SD-or-RAM, snapshot_history.h) and only ever
// accepts an integer age index, never a raw filename, so there's no new
// path-traversal surface to reason about here.
String renderGalleryPanel(const String& cameraFilter, std::vector<CameraConfig>* liveCameras,
                           std::vector<CameraState>* liveStates);
