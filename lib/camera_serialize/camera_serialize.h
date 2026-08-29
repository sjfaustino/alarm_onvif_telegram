#pragma once
#include <Arduino.h> // explicit, not just via camera_store.h - PlatformIO's LDF resolves a
                      // lib's dependencies by scanning its own #includes, not through a loose
                      // top-level header like camera_store.h
#include <vector>
#include "camera_store.h"

// Pure (de)serialization between CameraConfig and the pipe-delimited
// record format camera_store.cpp persists to NVS. Split out so it can be
// unit-tested natively (test/test_camera_serialize) without <Preferences.h>,
// which only exists on-device.
//
// Schema-versioned: camera_store.cpp stores CAMERA_SCHEMA_VERSION alongside
// the record blob and passes it back in on every load. Bump this constant -
// and add a new branch in deserializeCamera(), never edit an existing one -
// whenever serializeCamera()'s field layout changes. This is what prevents
// a field inserted mid-layout (rather than appended) from silently
// misparsing every existing saved camera: fields.size() tolerance alone
// can't tell "an old record missing trailing fields" apart from "a record
// whose fields moved."
static const uint16_t CAMERA_SCHEMA_VERSION = 2;

// Always writes CAMERA_SCHEMA_VERSION's current field layout.
String serializeCamera(const CameraConfig& c);

// `recordVersion` is the schema version `record` was saved under (0 =
// written before versioning existed, the original field-count-tolerant
// format). Returns a default-constructed CameraConfig (name.length()==0)
// if `record` is malformed for that version - camera_store.cpp's
// loadCameras() skips (and logs) any entry that comes back with an empty name.
CameraConfig deserializeCamera(const String& record, uint16_t recordVersion);

// Number of fields `record` splits into on FIELD_SEP - for
// camera_store.cpp's diagnostic log on a parse failure, not used by
// deserializeCamera itself.
size_t cameraRecordFieldCount(const String& record);

// Sorts in place by name, case-insensitively. Display order only - zero-
// padded numeric suffixes ("D01".."D09", "D10") already sort correctly as
// plain text, no natural-sort needed.
void sortCamerasByName(std::vector<CameraConfig>& cams);
