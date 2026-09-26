#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes
#include <vector>
#include "camera_store.h"

// CameraConfig <-> the pipe-delimited NVS record, tested natively.
//
// Schema-versioned: the version is stored with the records and passed back on
// load. When the field layout changes, bump CAMERA_SCHEMA_VERSION and add a
// new deserializeCamera branch - never edit an old one. Field-count checks
// alone can't tell a short old record from one whose fields moved.
static const uint16_t CAMERA_SCHEMA_VERSION = 8;

String serializeCamera(const CameraConfig& c);

// recordVersion 0 = pre-versioning format. Returns an empty-name config if
// malformed; loadCameras skips those.
CameraConfig deserializeCamera(const String& record, uint16_t recordVersion);

// For the parse-failure log only.
size_t cameraRecordFieldCount(const String& record);

// Case-insensitive, for display.
void sortCamerasByName(std::vector<CameraConfig>& cams);
