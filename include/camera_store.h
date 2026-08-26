#pragma once
#include <Arduino.h>
#include <vector>

// A camera's full configuration - persisted in NVS (Preferences, namespace
// "camstore") instead of a compile-time array, so cameras can be added and
// deleted at runtime via the web UI (see webserver.h). Changes take effect
// after a reboot - see webserver.cpp's top comment for why.
struct CameraConfig {
  String name;
  String deviceServiceUrl;
  bool   enabled = true;

  // useWSSecurity: most ONVIF stacks require a WS-Security digest header on
  // every request. Some cheaper/older stacks expect plain HTTP Basic Auth
  // instead and choke on - or just ignore - the WSSE header. Try ON first
  // on a new camera; flip OFF if every request comes back with an auth
  // fault.
  bool useWSSecurity = true;

  // includeInitialTerminationTime / includeReplyToAnonymous: workaround
  // toggles for CreatePullPointSubscription faults seen on several cameras
  // (Xiongmai-derived stacks and even a spec-correct ONVIF 2.40 device both
  // needed both OFF to fix a NotAuthorized fault that only showed up on
  // this one call). Try ON first as the spec-correct default; flip both OFF
  // if CreatePullPointSubscription specifically faults while every other
  // Events-service call succeeds. Some cameras (e.g. a Reolink that
  // defaults to a ~10s subscription lifetime when none is requested)
  // instead need includeInitialTerminationTime ON.
  bool includeInitialTerminationTime = false;
  bool includeReplyToAnonymous = false;

  // snapshotUriOverride: set only if the camera's own ONVIF GetSnapshotUri
  // response is broken/wrong and a working direct snapshot URL has been
  // found by hand. {USER} and {PASS} in the URL are substituted with
  // user/pass at runtime, for cameras that want credentials as query
  // params instead of HTTP Basic Auth. Leave empty to use the standard
  // ONVIF GetProfiles -> GetSnapshotUri flow.
  String snapshotUriOverride;

  // preferredProfileKeyword: when a camera exposes multiple profiles, this
  // is matched case-insensitively against each profile's <Name> to pick
  // which one to snapshot from. Leave empty to use whichever comes first.
  String preferredProfileKeyword;

  String user;
  String pass;

  // Minimum time between Telegram alerts for this camera - a motion event
  // that fires again before this elapses since the last alert is still
  // detected/logged, just not re-sent. Per-camera because a camera pointed
  // at a busy driveway wants a different cadence than one watching a quiet
  // back gate. See triggerMotionAlert in telegram.cpp.
  unsigned long alertCooldownMs = 30000;

  // How long this camera can go without answering any SOAP request before
  // it's flagged OFFLINE (and alerted on) - see checkCameraOnlineStatus in
  // telegram.cpp. Per-camera because a camera on a flaky/slow link
  // shouldn't be held to the same threshold as one on solid Ethernet-backed
  // WiFi right next to the AP.
  unsigned long offlineThresholdMs = 5UL * 60UL * 1000UL;

  // notes: free-text context for future-you - why a quirk flag is set the
  // way it is, what was tried. Shown in the web UI, never sent anywhere.
  String notes;

  // How many consecutive snapshots to fetch and send when motion fires -
  // each one its own fresh fetch (back to back, no artificial delay - the
  // fetch and the Telegram upload already take real time), captioned with
  // its own timestamp plus a "(n/N)" suffix once there's more than one.
  // Default 1 (today's single-snapshot behavior); raise it per-camera when
  // you want enough context after the fact to see *why* an alert fired, at
  // the cost of a slower, chattier alert.
  unsigned int snapshotBurstCount = 1;
};

// Loads the camera list from NVS. On the very first boot after upgrading to
// NVS-backed camera storage (nothing in NVS yet), seeds it once from
// CAMERA_SEED in secrets.h (gitignored - see secrets.h.example) so cameras
// already tuned before this change don't have to be re-entered by hand
// through the web UI. After that first boot, CAMERA_SEED is never read
// again - the web UI is the only way to add/remove cameras from then on.
std::vector<CameraConfig> loadCameras();

// Overwrites the entire persisted camera list.
bool saveCameras(const std::vector<CameraConfig>& cameras);

// Convenience wrappers used by the web UI - each loads, mutates, and saves
// in one call. addCamera fails (returns false) if a camera with that name
// already exists (name is the unique key cameras have always been matched
// by). deleteCamera fails if no camera with that name exists.
bool addCamera(const CameraConfig& cam);
bool deleteCamera(const String& name);

// Replaces the camera currently named originalName with cam - cam.name
// doesn't have to match originalName, so this also handles a rename. Fails
// (returns false, nothing saved) if originalName isn't found, or if
// cam.name collides with a *different* existing camera's name.
bool updateCamera(const String& originalName, const CameraConfig& cam);
