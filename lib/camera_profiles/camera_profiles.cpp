#include "camera_profiles.h"
#include "format_utils.h"

namespace {

// Add entries as models are verified on real hardware; say in the comment what
// was seen.
const CameraModelProfile kProfiles[] = {
    // H46: 704x576 JPEGs of ~85 KB were slow over a VPN link (2026-11-08).
    {"Vatilon", "H46", "/cgi-bin/snapshot.cgi", 500, -1,
     "Vatilon H46: on a slow link, lower the snapshot size in the camera's web UI "
     "(Configure > Stream Manager > Snapshot Setting, e.g. 640x360, quality Mid)."},
    // PB1, H44, H46, H80: GetSnapshotUri is blank or unreliable but
    // /cgi-bin/snapshot.cgi returns a JPEG; PullMessages ignores the long-poll
    // timeout and answers at once, so a 500 ms interval keeps event delay low.
    {"Vatilon", "", "/cgi-bin/snapshot.cgi", 500, -1, nullptr},
    // RLC-520A: without InitialTerminationTime it refused the subscription
    // every 12 s (D07, 2026-11-07).
    {"REOLINK", "RLC-520A", nullptr, 0, 1, nullptr},
    {"tp-link", "Tapo TC65", nullptr, 0, -1,
     "Tapo TC65: offers no ONVIF snapshot, so its motion alerts arrive as text."},
    // Vstarcam firmware 2.4 reports this generic identity.
    {"IP camera", "IP Camera", nullptr, 0, -1,
     "Generic \"IP Camera\" (Vstarcam firmware 2.4): rejects ONVIF event subscriptions, "
     "so it can't send motion alerts; snapshots and timelapse work."},
};

}  // namespace

const CameraModelProfile* findCameraModelProfile(const String& manufacturer, const String& model) {
  String m = manufacturer;
  m.trim();
  String mo = model;
  mo.trim();
  for (const CameraModelProfile& p : kProfiles) {
    if (!m.equalsIgnoreCase(p.manufacturer)) continue;
    if (p.model[0] != '\0' && !mo.equalsIgnoreCase(p.model)) continue;
    return &p;
  }
  return nullptr;
}

std::vector<String> applyCameraModelProfile(CameraConfig& cam, const CameraModelProfile& profile) {
  const CameraConfig defaults;
  std::vector<String> changes;

  if (profile.snapshotPath && cam.snapshotUriOverride.length() == 0) {
    String host = extractHost(cam.deviceServiceUrl);
    if (host.length() > 0) {
      cam.snapshotUriOverride = "http://" + host + profile.snapshotPath;
      changes.push_back("snapshot URL " + cam.snapshotUriOverride);
    }
  }
  if (profile.pollIntervalMs > 0 && cam.pollIntervalMs == defaults.pollIntervalMs &&
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
