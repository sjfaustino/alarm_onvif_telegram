#pragma once
#include <Arduino.h>
#include <vector>
#include <functional>
#include "config.h" // PULL_INTERVAL_MS - CameraConfig::pollIntervalMs's own default

// A camera's full configuration - persisted in NVS (Preferences, namespace
// "camstore") so cameras can be added/deleted at runtime via the web UI.
// Changes take effect after a reboot.
struct CameraConfig {
  String name;
  String deviceServiceUrl;
  bool   enabled = true;

  // Most ONVIF stacks need a WS-Security digest header; some cheaper/older
  // ones expect plain HTTP Basic Auth and choke on WSSE. Try ON first;
  // flip OFF if every request comes back with an auth fault.
  bool useWSSecurity = true;

  // Workarounds for CreatePullPointSubscription faults seen on several
  // cameras (Xiongmai-derived stacks, even a spec-correct ONVIF 2.40
  // device). Try ON first; flip both OFF if only this call faults while
  // other Events-service calls succeed. A camera with a very short default
  // subscription lifetime (some Reolinks: ~10s) instead needs
  // includeInitialTerminationTime ON.
  bool includeInitialTerminationTime = false;
  bool includeReplyToAnonymous = false;

  // Set only if the camera's own GetSnapshotUri response is broken and a
  // working URL was found by hand. {USER}/{PASS} are substituted at
  // runtime for cameras wanting credentials as query params. Empty uses
  // the standard GetProfiles -> GetSnapshotUri flow.
  String snapshotUriOverride;

  // Matched case-insensitively against each profile's <Name> to pick which
  // one to snapshot from when a camera exposes several. Empty = first one.
  String preferredProfileKeyword;

  String user;
  String pass;

  // Minimum time between alerts for this camera; a motion event within
  // this window is still detected/logged, just not re-sent.
  unsigned long alertCooldownMs = 30000;

  // How long this camera can go without answering a SOAP request before
  // it's flagged OFFLINE.
  unsigned long offlineThresholdMs = 5UL * 60UL * 1000UL;

  // Free-text context for future-you. Shown in the web UI, never sent anywhere.
  String notes;

  // Consecutive fresh snapshots to send per motion event (captioned
  // "(n/N)" once more than one).
  unsigned int snapshotBurstCount = 1;

  // Recurring daily do-not-disturb window - motion alerts only (tamper/
  // signal-loss stay always-on, see telegram.cpp's triggerMotionAlert).
  // quietStartMinute/quietEndMinute are minutes since local midnight
  // (0-1439). quietStartMinute == quietEndMinute means "no active window"
  // (see lib/quiet_hours' own comment for why that's the safe default,
  // not "always quiet").
  bool quietHoursEnabled = false;
  uint16_t quietStartMinute = 0;
  uint16_t quietEndMinute = 0;

  // Alerts (Telegram + Activity log) if this camera hasn't seen a real
  // motion event in over this many hours - catches a dead PIR or a camera
  // knocked to face the wrong way, which otherwise looks identical to "a
  // quiet day". 0 = off (default - many cameras legitimately go long
  // stretches without motion).
  uint16_t motionWatchdogHours = 0;

  // Captures one snapshot on this interval regardless of motion, stored
  // the same way as an alert snapshot (SD if active, RAM ring otherwise) -
  // never sent to Telegram. 0 = off (default).
  uint16_t timelapseIntervalMin = 0;

  // How often this camera's own task asks "anything new?" via ONVIF
  // PullMessages (camera.cpp's cameraTaskFn) - lower means motion is
  // noticed sooner (PullMessages' own PT1S long-poll is designed for
  // frequent re-polling), at the cost of more frequent SOAP round-trips to
  // this specific camera. Per-camera, not global, since cheaper embedded
  // HTTP stacks tolerate more frequent polling worse than others do.
  unsigned long pollIntervalMs = PULL_INTERVAL_MS;
};

// Loads the camera list from NVS, seeding once from CAMERA_SEED in
// secrets.h on the very first boot.
std::vector<CameraConfig> loadCameras();

// Overwrites the entire persisted camera list.
bool saveCameras(const std::vector<CameraConfig>& cameras);

// Convenience wrappers used by the web UI - load, mutate, save in one
// call. addCamera fails if the name already exists (the unique key
// cameras are matched by); deleteCamera fails if it doesn't.
bool addCamera(const CameraConfig& cam);
bool deleteCamera(const String& name);

// Replaces the camera named originalName with cam (cam.name need not
// match, so this also handles renames). Fails if originalName isn't
// found, or cam.name collides with a different existing camera.
bool updateCamera(const String& originalName, const CameraConfig& cam);

// Wholesale replace of the entire persisted list (config import - see
// webserver_security.cpp's applyConfigImport) - unlike calling saveCameras()
// directly, this takes the same mutex addCamera/updateCamera/deleteCamera
// do, so an import landing at the same moment as a concurrent dashboard
// edit can't lose either change to the other.
bool replaceAllCameras(const std::vector<CameraConfig>& cameras);

// Atomically loads every camera, applies `mutate` to each one in place
// (e.g. a bulk field change from the dashboard), and saves the whole list
// back - all under the same mutex addCamera/updateCamera/deleteCamera/
// replaceAllCameras use. Unlike calling loadCameras() and then
// replaceAllCameras() as two separate steps, which only protects the
// final write, the READ here is inside the same critical section too - a
// concurrent single-camera edit landing between an unprotected read and a
// protected write would otherwise get silently discarded when the stale
// pre-edit list gets written back over it. Returns false (nothing
// touched) if there are no cameras to begin with, or the save itself fails.
bool updateAllCameras(const std::function<void(CameraConfig&)>& mutate);

// One-time recovery path: adds any CAMERA_SEED (secrets.h) entry whose
// name doesn't already exist in the persisted list - unlike the
// first-boot seed in loadCameras(), this runs even when the store is
// already initialized, so it can restore cameras lost to a bug without
// touching whatever's already there or duplicating by name. Gated by its
// own NVS flag so it only ever actually does something once, even across
// reboots - safe to call unconditionally from setup(). Returns how many
// cameras it added.
size_t restoreMissingCamerasFromSeed();
