#pragma once
#include <Arduino.h>
#include <vector>
#include "camera_store.h"

// Known settings per camera model, applied once when a camera is added: the
// board reads the camera's Manufacturer and Model (ONVIF GetDeviceInformation)
// and fills in the matching entry. Only fields still at their defaults are
// filled, so anything the user typed in the add form wins, and later edits are
// never touched. Pure and tested natively.

struct CameraModelProfile {
  // Matched case-insensitively. An empty model matches any model from that
  // manufacturer; entries are tried in order, so specific models go first.
  const char* manufacturer;
  const char* model;
  // Each is applied only when set (non-null / non-zero / not -1).
  const char* snapshotPath;           // e.g. "/cgi-bin/snapshot.cgi", on the device service's host
  unsigned long pollIntervalMs;       // 0 = leave as is
  int8_t includeInitialTerminationTime; // -1 = leave as is
  // Something the board can't set itself, e.g. a setting in the camera's own
  // web UI. Appended to the camera's notes.
  const char* tip;
};

// The matching entry, or nullptr.
const CameraModelProfile* findCameraModelProfile(const String& manufacturer, const String& model);

// Applies `profile` to the fields of `cam` still at their defaults. Returns one
// line per change, for the log; empty if nothing changed.
std::vector<String> applyCameraModelProfile(CameraConfig& cam, const CameraModelProfile& profile);
