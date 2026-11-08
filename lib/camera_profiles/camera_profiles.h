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
  bool known = false;
  // Each is applied only when set.
  const char* snapshotPath = nullptr;       // e.g. "/cgi-bin/snapshot.cgi", on the device service's host
  unsigned long pollIntervalMs = 0;         // 0 = leave as is
  int8_t includeInitialTerminationTime = -1; // -1 = leave as is
  // Something the board can't set itself, e.g. a setting in the camera's own
  // web UI. Appended to the camera's notes.
  const char* tip = nullptr;
};

// Matched case-insensitively; .known is false if the model isn't listed.
CameraModelProfile findCameraModelProfile(const String& manufacturer, const String& model);

// Applies `profile` to the fields of `cam` still at their defaults. Returns one
// line per change, for the log; empty if nothing changed. A camera off the
// local subnet may be behind a metered link, so it keeps the default poll
// interval.
std::vector<String> applyCameraModelProfile(CameraConfig& cam, const CameraModelProfile& profile, bool onLocalSubnet);

// Whether `host` (an IPv4 address, optionally ":port") is on the subnet of
// localIp. Addresses are most-significant-octet first. A hostname counts as
// not local, since its address isn't known here.
bool hostIsOnSubnet(const String& host, uint32_t localIp, uint32_t subnetMask);
