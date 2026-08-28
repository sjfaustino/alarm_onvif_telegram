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
//   /on|/off <name/prefix> [duration] - resume/mute alerts (subscription
//                              stays up either way). Optional trailing
//                              duration schedules an automatic revert back
//                              to the opposite state - see
//                              parseDurationToken (telegram_parse.h) for
//                              exactly what it accepts (plain minutes, or
//                              a 24h "HH:MM" clock time). Omitted entirely
//                              means permanent, the original behavior.
//   /snap <name/prefix>      - fresh snapshot now, ignoring mute/cooldown
//   /status                  - list every enabled camera's on/off state
//   /uptime                  - board uptime
//   /reset                   - reboot the board immediately
// /on, /off, /status, /uptime require canCommand; /snap requires canSnap;
// /reset requires canReset (off by default, even for the seeded Admin user
// - see TelegramUser::canReset).
void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras);

// Flips alertsEnabled back for any camera whose timed /on or /off (see
// pollTelegramCommands) has reached its scheduled revert time - call once
// per loop() tick (main.cpp), same cadence as pollTelegramCommands itself.
// Cheap when nothing's due: just a millis() comparison per camera.
void checkScheduledAlertReverts(const CameraConfig cameras[], CameraState states[], size_t numCameras);
