#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes
#include "camera_store.h"
#include "webserver_html.h" // FormParams

// The camera Add/Edit form: rendering and parsing, kept pure so both can be
// tested natively.

// Minutes since midnight <-> "HH:MM" (for <input type="time">).
String minutesToHHMM(uint16_t minutes);
uint16_t parseHHMMToMinutes(const String& hhmm);

String renderCameraForm(const CameraConfig& v, bool isEdit);
CameraConfig parseCameraForm(const FormParams& params);
