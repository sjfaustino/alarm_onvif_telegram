#pragma once
#include <Arduino.h>
#include <vector>
#include "camera.h"

// Gallery panel. cameraFilter "" shows the camera picker; otherwise that
// camera's thumbnails, GALLERY_PAGE_SIZE per page (0 = newest). sourceFilter
// (a snapshotSourceLabel) and dateFilter ("YYYYMMDD", SD only) narrow the grid
// and combine.
//
// Images go through the existing /cameras/snapshot?name=&age= route, which
// only takes an index - no new SD path, so no path-traversal surface.
String renderGalleryPanel(const String& cameraFilter, size_t page, const String& sourceFilter,
                           const String& dateFilter, std::vector<CameraConfig>* liveCameras,
                           std::vector<CameraState>* liveStates);
