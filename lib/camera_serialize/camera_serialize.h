#pragma once
#include <Arduino.h> // explicit, not just via camera_store.h - PlatformIO's LDF resolves a
                      // lib's dependencies (e.g. ArduinoFake for env:native) by scanning that
                      // lib's own #includes, and doesn't chain through camera_store.h since
                      // it's a loose top-level header (found via -Iinclude) rather than a
                      // library of its own
#include "camera_store.h"

// Pure (de)serialization between CameraConfig and the pipe-delimited
// record format camera_store.cpp persists to NVS (one record per camera,
// joined by camera_store.cpp's own RECORD_SEP). Split out from
// camera_store.cpp so it can be unit-tested natively
// (test/test_camera_serialize) without pulling in <Preferences.h>, which
// only exists on-device - and so a change to the field layout gets a
// failing round-trip test instead of silently misparsing every existing
// camera's NVS record the next time someone reboots.
String serializeCamera(const CameraConfig& c);

// Returns a default-constructed CameraConfig (name.length()==0) if
// `record` is malformed - camera_store.cpp's loadCameras() skips any
// entry that comes back with an empty name.
CameraConfig deserializeCamera(const String& record);
