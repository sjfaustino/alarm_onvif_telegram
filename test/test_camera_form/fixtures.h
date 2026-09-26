#pragma once
#include <Arduino.h>
#include <map>
#include <string>
#include "camera_form.h"

// Configs the golden tests render. Each exercises different branches: the
// blank Add form, an Edit with every field non-default and HTML-hostile
// text, and the quiet-hours/watchdog warning (same-day and wrapping).

inline CameraConfig fixtureAddDefault() { return CameraConfig(); }

inline CameraConfig fixtureEditHostile() {
  CameraConfig c;
  c.name = "D01 <\"Front\" & 'Door'>";
  c.deviceServiceUrl = "http://192.168.1.50:8080/onvif/device_service?a=1&b=<2>";
  c.user = "admin\"x";
  c.pass = "secret";
  c.enabled = false;
  c.useWSSecurity = false;
  c.includeInitialTerminationTime = true;
  c.includeReplyToAnonymous = true;
  c.snapshotUriOverride = "http://cam/snap.jpg?user={USER}&pass={PASS}&w={WIDTH}";
  c.snapshotMaxWidth = 640;
  c.snapshotMaxHeight = 480;
  c.preferredProfileKeyword = "sub\"";
  c.alertCooldownMs = 45000;
  c.offlineThresholdMs = 10UL * 60000UL;
  c.snapshotBurstCount = 3;
  c.pollIntervalMs = 750;
  c.quietHoursEnabled = true;
  c.quietStartMinute = 9 * 60 + 30;
  c.quietEndMinute = 17 * 60 + 5;
  c.motionWatchdogHours = 2;  // shorter than the 7h35m window -> warning
  c.timelapseIntervalMin = 15;
  c.timelapseSendToTelegram = true;
  c.retentionDays = 90;
  c.personAlertsEnabled = false;
  c.vehicleAlertsEnabled = false;
  c.petAlertsEnabled = true;
  c.petAlertsTextOnly = true;
  c.motionDigestEnabled = false;
  c.notes = "gate <b>side</b>";
  return c;
}

inline CameraConfig fixtureWrapNoWarning() {
  CameraConfig c;
  c.name = "Back";
  c.deviceServiceUrl = "http://10.0.0.9/onvif/device_service";
  c.quietHoursEnabled = true;
  c.quietStartMinute = 22 * 60;
  c.quietEndMinute = 6 * 60;  // 8h window across midnight
  c.motionWatchdogHours = 9;  // longer -> no warning
  return c;
}

inline CameraConfig fixtureWrapWarning() {
  CameraConfig c = fixtureWrapNoWarning();
  c.motionWatchdogHours = 8;  // equal to the window -> warning
  return c;
}

// Map-backed FormParams for parse tests.
class MapParams : public FormParams {
 public:
  std::map<std::string, std::string> values;
  bool has(const char* name) const override { return values.count(name) > 0; }
  String get(const char* name, const char* fallback) const override {
    auto it = values.find(name);
    return String(it == values.end() ? fallback : it->second.c_str());
  }
};
