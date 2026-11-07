#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes
#include "camera_store.h"

// Limits re-applied wherever a stored camera setting is used, because imported
// configs bypass the dashboard form.
unsigned long safeAlertCooldownMs(const CameraConfig& cfg);
unsigned long safeOfflineThresholdMs(const CameraConfig& cfg);
unsigned int safeSnapshotBurstCount(const CameraConfig& cfg);
uint16_t safeSnapshotDimension(uint16_t value);
unsigned long safePollIntervalMs(const CameraConfig& cfg);
