#include "camera_profiles.h"
#include "format_utils.h"

namespace {

struct SettingsRow {
  const char* manufacturer;
  const char* model; // "" = any model from that manufacturer
  const char* snapshotPath;
  unsigned long pollIntervalMs;
  int8_t includeInitialTerminationTime;
};

struct TipRow {
  const char* manufacturer;
  const char* model;
  const char* tip;
};

// Add entries as models are verified on real hardware; say in the comment what
// was seen. The first match in each table is used.
const SettingsRow kSettings[] = {
    // PB1, H44, H46, H80: GetSnapshotUri is blank or unreliable but
    // /cgi-bin/snapshot.cgi returns a JPEG; PullMessages ignores the long-poll
    // timeout and answers at once, so a 500 ms interval keeps event delay low.
    {"Vatilon", "", "/cgi-bin/snapshot.cgi", 500, -1},
    // RLC-520A: without InitialTerminationTime it refuses the subscription
    // every 12 s.
    {"REOLINK", "RLC-520A", nullptr, 0, 1},
};

const TipRow kTips[] = {
    // 704x576 JPEGs of ~85 KB are slow over a VPN link.
    {"Vatilon", "H46",
     "Vatilon H46: on a slow link, lower the snapshot size in the camera's web UI "
     "(Configure > Stream Manager > Snapshot Setting, e.g. 640x360, quality Mid)."},
    {"tp-link", "Tapo TC65", "Tapo TC65: offers no ONVIF snapshot, so its motion alerts arrive as text."},
    // Vstarcam firmware 2.4 reports this generic identity.
    {"IP camera", "IP Camera",
     "Generic \"IP Camera\" (Vstarcam firmware 2.4): rejects ONVIF event subscriptions, "
     "so it can't send motion alerts; snapshots and timelapse work."},
};

bool matches(const char* wantManufacturer, const char* wantModel, const String& manufacturer, const String& model) {
  if (!manufacturer.equalsIgnoreCase(wantManufacturer)) return false;
  return wantModel[0] == '\0' || model.equalsIgnoreCase(wantModel);
}

}  // namespace

CameraModelProfile findCameraModelProfile(const String& manufacturer, const String& model) {
  String m = manufacturer;
  m.trim();
  String mo = model;
  mo.trim();

  CameraModelProfile profile;
  for (const SettingsRow& row : kSettings) {
    if (!matches(row.manufacturer, row.model, m, mo)) continue;
    profile.known = true;
    profile.snapshotPath = row.snapshotPath;
    profile.pollIntervalMs = row.pollIntervalMs;
    profile.includeInitialTerminationTime = row.includeInitialTerminationTime;
    break;
  }
  for (const TipRow& row : kTips) {
    if (!matches(row.manufacturer, row.model, m, mo)) continue;
    profile.known = true;
    profile.tip = row.tip;
    break;
  }
  return profile;
}

std::vector<String> applyCameraModelProfile(CameraConfig& cam, const CameraModelProfile& profile, bool onLocalSubnet) {
  const CameraConfig defaults;
  std::vector<String> changes;

  if (profile.snapshotPath && cam.snapshotUriOverride.length() == 0) {
    String host = extractHost(cam.deviceServiceUrl);
    if (host.length() > 0) {
      cam.snapshotUriOverride = "http://" + host + profile.snapshotPath;
      changes.push_back("snapshot URL " + cam.snapshotUriOverride);
    }
  }
  if (onLocalSubnet && profile.pollIntervalMs > 0 && cam.pollIntervalMs == defaults.pollIntervalMs &&
      profile.pollIntervalMs != defaults.pollIntervalMs) {
    cam.pollIntervalMs = profile.pollIntervalMs;
    changes.push_back("poll interval " + String(profile.pollIntervalMs) + " ms");
  }
  if (profile.includeInitialTerminationTime >= 0 &&
      cam.includeInitialTerminationTime == defaults.includeInitialTerminationTime &&
      (profile.includeInitialTerminationTime == 1) != defaults.includeInitialTerminationTime) {
    cam.includeInitialTerminationTime = profile.includeInitialTerminationTime == 1;
    changes.push_back(String("InitialTerminationTime ") + (cam.includeInitialTerminationTime ? "on" : "off"));
  }
  if (profile.tip && cam.notes.indexOf(profile.tip) < 0) {
    cam.notes = cam.notes.length() > 0 ? cam.notes + " " + profile.tip : String(profile.tip);
    changes.push_back("tip added to notes");
  }
  return changes;
}

bool hostIsOnSubnet(const String& host, uint32_t localIp, uint32_t subnetMask) {
  uint32_t ip = 0;
  int octets = 0, value = -1;
  for (size_t i = 0; i <= host.length(); i++) {
    char c = i < host.length() ? host[i] : '.';
    if (c == ':' && octets == 3) c = '.', i = host.length();
    if (c >= '0' && c <= '9') {
      value = (value < 0 ? 0 : value) * 10 + (c - '0');
      if (value > 255) return false;
    } else if (c == '.' && value >= 0 && octets < 4) {
      ip = (ip << 8) | (uint32_t)value;
      octets++;
      value = -1;
    } else {
      return false;
    }
  }
  return octets == 4 && (ip & subnetMask) == (localIp & subnetMask);
}
