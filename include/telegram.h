#pragma once
#include <Arduino.h>
#include "config.h"
#include "camera.h"

// Sends cfg.snapshotBurstCount snapshot(s), captioned "<camera name> -
// <UTC timestamp>" (plus "(n/N)"), to every subscribed user, subject to
// cfg's alert cooldown. Safe to call on every motion event.
void triggerMotionAlert(const CameraConfig& cfg, CameraState& st);

// Broadcasts an OFFLINE/back-ONLINE notice on a lastContactMs/offlineThresholdMs
// state transition. Cheap enough to call every cameraTaskFn loop iteration.
void checkCameraOnlineStatus(const CameraConfig& cfg, CameraState& st);

// Sends text to every user with systemMessages enabled. Returns false if
// no user has it enabled, or every send failed.
bool sendTelegramMessage(const String& text);

// True once TELEGRAM_ROOT_CA holds a real certificate - false means every
// send will fail TLS verification.
bool telegramCAConfigured();

// Reads camera `index`'s persisted alerts-enabled flag from NVS (default
// true). Call once per camera at boot, before spawning its task.
bool loadAlertEnabledPref(size_t index);

// Polls getUpdates and applies commands, matched by case-insensitive
// camera-name prefix ("/on D01" matches "D01-FDir"; an ambiguous prefix
// lists the matches instead of applying anything):
//   /on|/off <name/prefix>   - resume/mute alerts (subscription stays up)
//   /snap <name/prefix>      - fresh snapshot now, ignoring mute/cooldown
//   /status                  - list every enabled camera's on/off state
// /on, /off, /status require canCommand; /snap requires canSnap.
void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras);
