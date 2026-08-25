#pragma once
#include <Arduino.h>
#include "config.h"
#include "camera.h"

// Fetches a snapshot JPEG from st.snapshotUri, sends it to Telegram with a
// caption of "<camera name> - <UTC timestamp>", subject to cfg's per-camera
// alert cooldown. Safe to call on every motion event; it self-throttles.
void triggerMotionAlert(const CameraConfig& cfg, CameraState& st);
