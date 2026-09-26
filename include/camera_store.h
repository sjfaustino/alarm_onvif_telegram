#pragma once
#include <Arduino.h>
#include <vector>
#include <functional>
#include "config.h" // PULL_INTERVAL_MS - CameraConfig::pollIntervalMs's own default

// A camera's configuration, persisted in NVS ("camstore") and edited from the
// dashboard.
struct CameraConfig {
  String name;
  String deviceServiceUrl;
  bool   enabled = true;

  // Most ONVIF stacks need a WS-Security digest header; some cheaper/older
  // ones expect plain HTTP Basic Auth and choke on WSSE. Try ON first;
  // flip OFF if every request comes back with an auth fault.
  bool useWSSecurity = true;

  // Workarounds for CreatePullPointSubscription faults on some stacks
  // (Xiongmai-derived, even a spec-correct 2.40 device): try ON, flip both OFF
  // if only this call faults. Cameras with a very short default lifetime (some
  // Reolinks, ~10s) need includeInitialTerminationTime ON instead.
  bool includeInitialTerminationTime = false;
  bool includeReplyToAnonymous = false;

  // Hand-found snapshot URL for cameras whose GetSnapshotUri is broken; empty
  // uses the standard flow. {USER}/{PASS} (and {WIDTH}/{HEIGHT}) are
  // substituted at runtime.
  String snapshotUriOverride;

  // Matched case-insensitively against each profile's <Name> to pick which
  // one to snapshot from when a camera exposes several. Empty = first one.
  String preferredProfileKeyword;

  String user;
  String pass;

  // Minimum time between alerts; events inside it are still logged.
  unsigned long alertCooldownMs = 30000;

  // Silence before the camera is flagged OFFLINE.
  unsigned long offlineThresholdMs = 5UL * 60UL * 1000UL;

  // Shown in the web UI only.
  String notes;

  // Consecutive fresh snapshots to send per motion event (captioned
  // "(n/N)" once more than one).
  unsigned int snapshotBurstCount = 1;

  // Daily do-not-disturb window for motion alerts (tamper/signal loss always
  // alert). Minutes since local midnight; start == end means no window.
  bool quietHoursEnabled = false;
  uint16_t quietStartMinute = 0;
  uint16_t quietEndMinute = 0;

  // Alert if no real motion for this many hours (dead PIR, camera knocked
  // askew). 0 = off.
  uint16_t motionWatchdogHours = 0;

  // Periodic snapshot regardless of motion, stored like alert snapshots. 0 =
  // off.
  uint16_t timelapseIntervalMin = 0;

  // Also send timelapse captures to Telegram, with a routine caption.
  bool timelapseSendToTelegram = false;

  // Per-camera retention in days; 0 = use the global Storage setting.
  uint16_t retentionDays = 0;

  // Opt-in: pets would otherwise alert on every trip through the yard.
  bool petAlertsEnabled = false;

  // Send pet alerts as text only, without photos.
  bool petAlertsTextOnly = false;

  // PullMessages cadence. Lower notices motion sooner but loads the camera;
  // cheap HTTP stacks tolerate less.
  unsigned long pollIntervalMs = PULL_INTERVAL_MS;

  // {WIDTH}/{HEIGHT} values for snapshotUriOverride, to request a smaller
  // snapshot without changing the camera's encoder settings. 0 = unset.
  uint16_t snapshotMaxWidth = 0;
  uint16_t snapshotMaxHeight = 0;

  // Opt-out: person/vehicle detection is the core security signal, so it must
  // never silently turn off. For e.g. a camera facing a busy street. Only
  // affects cameras whose AI classifies person/vehicle.
  bool personAlertsEnabled = true;
  bool vehicleAlertsEnabled = true;

  // Opt-out of the "motion continued" follow-up after a cooldown, for cameras
  // where it's just noise.
  bool motionDigestEnabled = true;
};

// Loads from NVS, seeding from secrets.h's CAMERA_SEED on first boot.
std::vector<CameraConfig> loadCameras();

// Overwrites the entire persisted camera list.
bool saveCameras(const std::vector<CameraConfig>& cameras);

// Load-mutate-save helpers. Cameras are keyed by name: add fails on a
// duplicate, delete on a missing name.
bool addCamera(const CameraConfig& cam);
bool deleteCamera(const String& name);

// Replaces originalName with cam (handles renames). Fails if not found or
// cam.name collides.
bool updateCamera(const String& originalName, const CameraConfig& cam);

// Replaces the whole list under the store mutex (config import), so it can't
// race a dashboard edit.
bool replaceAllCameras(const std::vector<CameraConfig>& cameras);

// Read-modify-write of every camera inside one critical section, so a
// concurrent single-camera edit isn't overwritten. False if there are no
// cameras or the save fails.
bool updateAllCameras(const std::function<void(CameraConfig&)>& mutate);

// One-time recovery: adds CAMERA_SEED entries missing by name, even on an
// initialised store. Gated by an NVS flag, so safe to call every boot. Returns
// the number added.
size_t restoreMissingCamerasFromSeed();
