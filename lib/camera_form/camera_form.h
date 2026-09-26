#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes
#include "camera_store.h"

// The camera Add/Edit form: rendering and parsing, kept pure so both can be
// tested natively.

// Submitted form fields. The dashboard wraps a PsychicRequest; tests use a
// map.
class FormParams {
 public:
  virtual ~FormParams() = default;
  virtual bool has(const char* name) const = 0;
  virtual String get(const char* name, const char* fallback) const = 0;
};

// Minutes since midnight <-> "HH:MM" (for <input type="time">).
String minutesToHHMM(uint16_t minutes);
uint16_t parseHHMMToMinutes(const String& hhmm);

String renderCameraForm(const CameraConfig& v, bool isEdit);
CameraConfig parseCameraForm(const FormParams& params);
