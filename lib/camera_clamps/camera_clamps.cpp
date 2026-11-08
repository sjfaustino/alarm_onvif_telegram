#include "camera_clamps.h"
#include "config.h"

// Clamped at use (imported configs bypass the form). 0 isn't "off": it would
// alert on every poll with no digest throttling - the multi-camera burst that
// starves TLS of memory.
unsigned long safeAlertCooldownMs(const CameraConfig& cfg) {
  unsigned long ms = cfg.alertCooldownMs;
  if (ms == 0) ms = CameraConfig().alertCooldownMs;
  if (ms > CAMERA_ALERT_COOLDOWN_MAX_MS) ms = CAMERA_ALERT_COOLDOWN_MAX_MS;
  return ms;
}

// 0 would mark every camera OFFLINE permanently.
unsigned long safeOfflineThresholdMs(const CameraConfig& cfg) {
  unsigned long ms = cfg.offlineThresholdMs;
  if (ms == 0) ms = CameraConfig().offlineThresholdMs;
  if (ms > CAMERA_OFFLINE_THRESHOLD_MAX_MS) ms = CAMERA_OFFLINE_THRESHOLD_MAX_MS;
  return ms;
}

// Shots go out back-to-back, so an unclamped count floods Telegram.
unsigned int safeSnapshotBurstCount(const CameraConfig& cfg) {
  unsigned int shots = (cfg.snapshotBurstCount > 0) ? cfg.snapshotBurstCount : 1;
  if (shots > CAMERA_SNAPSHOT_BURST_MAX) shots = CAMERA_SNAPSHOT_BURST_MAX;
  return shots;
}

// Clamped at use; 0 means unset.
uint16_t safeSnapshotDimension(uint16_t value) {
  if (value == 0) return 0;
  if (value > CAMERA_SNAPSHOT_DIMENSION_MAX) return CAMERA_SNAPSHOT_DIMENSION_MAX;
  return value;
}

// Clamped at use. Near 0 would open a new connection ("Connection: close") in
// a tight loop.
unsigned long safePollIntervalMs(const CameraConfig& cfg) {
  unsigned long ms = cfg.pollIntervalMs;
  if (ms < CAMERA_POLL_INTERVAL_MIN_MS) ms = CAMERA_POLL_INTERVAL_MIN_MS;
  if (ms > CAMERA_POLL_INTERVAL_MAX_MS) ms = CAMERA_POLL_INTERVAL_MAX_MS;
  return ms;
}
