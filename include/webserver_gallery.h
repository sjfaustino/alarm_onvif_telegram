#pragma once
#include <Arduino.h>
#include <vector>
#include "camera.h"

// Gallery panel content: browse a camera's stored snapshot history beyond
// the Cameras page's 5-entry Preview strip. cameraFilter == "" renders the
// camera picker (every camera with any history); a non-empty value
// renders that camera's thumbnail grid, GALLERY_PAGE_SIZE entries at a
// time - page 0 is the newest, with "Newer"/"Older" links to move through
// the rest (ignored entirely when cameraFilter == ""). sourceFilter == ""
// shows every stored snapshot regardless of what triggered it; a
// snapshotSourceLabel value (snapshot_source.h - "person", "tamper", etc.)
// restricts the grid (and its own pagination) to just that kind, via a
// filter bar of only the kinds this camera actually has any of.
// dateFilter == "" shows every date; a "YYYYMMDD" value restricts to just
// that capture day, via a similar bar of only the distinct dates this
// camera actually has any of (SD-backed history only - the PSRAM ring has
// no wall-clock date to offer). Both filters combine (AND), each with its
// own independent link so picking one never drops the other. Split out of
// webserver.cpp - see webserver_network.h's comment for why.
//
// Deliberately reuses the existing /cameras/snapshot?name=&age= route
// (webserver.cpp) for every thumbnail/full-size image instead of adding a
// new SD read path - that route already dispatches through
// readCameraSnapshot (SD-or-RAM, snapshot_history.h) and only ever
// accepts an integer age index, never a raw filename, so there's no new
// path-traversal surface to reason about here.
String renderGalleryPanel(const String& cameraFilter, size_t page, const String& sourceFilter,
                           const String& dateFilter, std::vector<CameraConfig>* liveCameras,
                           std::vector<CameraState>* liveStates);
