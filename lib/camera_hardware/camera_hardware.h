#pragma once
#include <Arduino.h>
#include <vector>

// What a camera reports about itself over ONVIF, for the Hardware > Cameras
// page. Each parser fills only the fields its response carries and leaves the
// rest untouched; a field the camera doesn't report stays "". Pure and tested
// natively against real cameras' responses.

struct CameraHardwareInfo {
  // GetDeviceInformation
  String manufacturer, model, firmwareVersion, serialNumber, hardwareId;
  // GetNetworkInterfaces (first interface)
  String macAddress, ipv4, ipMode, link; // ipMode "DHCP"/"static"; link e.g. "100 Mbit/s, full duplex"
  // GetScopes, comma-joined where a camera lists several
  String scopeName, scopeLocation, scopeHardware;
  // GetSystemDateAndTime
  String timeSource, timeZone; // timeSource "NTP"/"Manual"
  long long clockUtcEpoch = 0; // 0 = not reported
  // GetProfiles, one line per profile, e.g. "MainStream: H264 1920x1080, 20 fps, 2173 kbit/s"
  std::vector<String> videoStreams;
};

void parseDeviceInformation(const String& xml, CameraHardwareInfo& info);
void parseNetworkInterfaces(const String& xml, CameraHardwareInfo& info);
void parseScopes(const String& xml, CameraHardwareInfo& info);
void parseSystemDateAndTime(const String& xml, CameraHardwareInfo& info);
void parseVideoStreams(const String& xml, CameraHardwareInfo& info);

// Seconds since 1970 for a UTC date and time; no range checks.
long long utcEpochSeconds(int year, int month, int day, int hour, int minute, int second);
