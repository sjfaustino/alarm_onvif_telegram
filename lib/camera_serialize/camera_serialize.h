#pragma once
#include <Arduino.h> // explicit, not just via camera_store.h - PlatformIO's LDF resolves a
                      // lib's dependencies (e.g. ArduinoFake for env:native) by scanning that
                      // lib's own #includes, and doesn't chain through camera_store.h since
                      // it's a loose top-level header (found via -Iinclude) rather than a
                      // library of its own
#include <vector>
#include "camera_store.h"

// Pure (de)serialization between CameraConfig and the pipe-delimited
// record format camera_store.cpp persists to NVS (one record per camera,
// joined by camera_store.cpp's own RECORD_SEP). Split out from
// camera_store.cpp so it can be unit-tested natively
// (test/test_camera_serialize) without pulling in <Preferences.h>, which
// only exists on-device.
//
// Schema-versioned: camera_store.cpp stores CAMERA_SCHEMA_VERSION
// alongside the record blob (a separate NVS key) and passes it back in on
// every load. Bump this constant - and add a new dedicated branch in
// deserializeCamera(), never edit an existing one - any time
// serializeCamera()'s field layout changes (added, removed, or reordered
// field). This is what actually prevents the failure mode a positional
// format is otherwise most at risk of: a field inserted in the *middle*
// of the layout instead of appended at the end would previously have been
// silently misparsed into wrong values for every existing saved camera,
// with no error - fields.size() tolerance alone can't tell "an old record
// missing trailing fields" apart from "a record whose fields just moved."
// Versioning makes that distinction explicit instead of assumed.
static const uint16_t CAMERA_SCHEMA_VERSION = 2;

// Always writes CAMERA_SCHEMA_VERSION's current field layout.
String serializeCamera(const CameraConfig& c);

// `recordVersion` is the schema version `record` was actually saved
// under (0 means "written before this versioning scheme existed" - the
// original field-count-tolerant format this project shipped with for a
// while, kept working here for anyone upgrading from it). Returns a
// default-constructed CameraConfig (name.length()==0) if `record` is
// malformed *for that version* - camera_store.cpp's loadCameras() skips
// (and logs) any entry that comes back with an empty name.
CameraConfig deserializeCamera(const String& record, uint16_t recordVersion);

// Number of fields `record` splits into on FIELD_SEP - for camera_store.cpp's
// diagnostic log when a record fails to parse, not used by deserializeCamera
// itself.
size_t cameraRecordFieldCount(const String& record);

// Sorts in place by name, case-insensitively. Display/iteration order
// only - doesn't touch NVS, and doesn't need natural/numeric-aware
// comparison: zero-padded numeric suffixes ("D01".."D09", "D10") already
// sort correctly as plain text.
void sortCamerasByName(std::vector<CameraConfig>& cams);
