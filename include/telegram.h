#pragma once
#include <Arduino.h>
#include "config.h"
#include "camera.h"

// Fetches a snapshot JPEG from st.snapshotUri, sends it to Telegram with a
// caption of "<camera name> - <UTC timestamp>", subject to cfg's per-camera
// alert cooldown. Safe to call on every motion event; it self-throttles.
void triggerMotionAlert(const CameraConfig& cfg, CameraState& st);

// Sends a plain text message to TELEGRAM_CHAT_ID (no photo attached) - used
// for the boot-time "camera monitor is online" notice, but generic enough
// for any future status/error message. Returns false on any failure
// (connect, write, or a non-200 Telegram response).
bool sendTelegramMessage(const String& text);

// True once TELEGRAM_ROOT_CA (telegram_ca.h) has been filled in with a real
// certificate. False means it's still the placeholder - every Telegram send
// will fail TLS verification until it's replaced. Checked once at boot so
// this is diagnosed with a clear log line instead of a stream of opaque
// "Could not connect" errors.
bool telegramCAConfigured();

// Reads camera `index`'s persisted alerts-enabled flag from NVS (Preferences,
// namespace "camctl") - defaults to true (ON) if never set. Call once per
// camera at boot, before spawning its task, so CameraState::alertsEnabled
// starts in whatever state a previous /on or /off command left it in.
bool loadAlertEnabledPref(size_t index);

// Polls Telegram's getUpdates for new commands from TELEGRAM_CHAT_ID and
// applies them to states[]/cameras[] (matched by CameraConfig::name):
//   /on <camera name>   - resume Telegram alerts for that camera
//   /off <camera name>  - mute Telegram alerts for that camera (polling and
//                         its ONVIF subscription keep running regardless)
//   /status             - list every enabled camera's current on/off state
// Each toggle is persisted via NVS so it survives a reboot. Commands from any
// chat ID other than TELEGRAM_CHAT_ID are ignored. Call periodically from
// loop() (e.g. every TELEGRAM_COMMAND_POLL_MS) - each call is one short,
// non-blocking-long-poll HTTPS round trip, safe to call often.
void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras);
