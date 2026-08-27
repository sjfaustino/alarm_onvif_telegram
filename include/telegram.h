#pragma once
#include <Arduino.h>
#include "config.h"
#include "camera.h"

// Fetches cfg.snapshotBurstCount snapshot(s) from st.snapshotUri (default
// 1; each shot is its own fresh fetch, back to back with no artificial
// delay - the fetch and the Telegram upload each already take a real
// amount of time) and sends each, captioned "<camera name> - <UTC
// timestamp>" (plus "(n/N)" once there's more than one), to every Telegram
// user (see telegram_users.h) subscribed to this camera - subject to
// cfg's per-camera alert cooldown. Safe to call on every motion event; it
// self-throttles.
void triggerMotionAlert(const CameraConfig& cfg, CameraState& st);

// Compares st.lastContactMs (updated on every SOAP response this camera
// sends back - see cameraSoapCall in camera.cpp) against cfg.offlineThresholdMs
// and, only on a state transition, broadcasts an OFFLINE or back-ONLINE
// notice via sendTelegramMessage. Cheap enough to call on every iteration
// of cameraTaskFn's loop - it only does anything (and only logs/sends) when
// st.isOffline actually flips.
void checkCameraOnlineStatus(const CameraConfig& cfg, CameraState& st);

// Sends a plain text message (no photo attached) to every Telegram user
// with systemMessages enabled - used for the boot-time "camera monitor is
// online" notice and the periodic heartbeat, but generic enough for any
// future broadcast. Returns false if no user has systemMessages enabled, or
// if every send to one that does failed.
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

// Polls Telegram's getUpdates for new commands and applies them to
// states[]/cameras[] (matched by CameraConfig::name, case-insensitive
// prefix - "/on D01" matches "D01-FDir" without typing the full name;
// replies with the ambiguity if a prefix matches more than one camera):
//   /on <camera name/prefix>   - resume Telegram alerts for that camera
//   /off <camera name/prefix>  - mute Telegram alerts for that camera
//                                (polling and its ONVIF subscription keep
//                                running regardless)
//   /snap <camera name/prefix> - fetch and send a fresh snapshot right now,
//                                to whoever asked only - ignores alertsEnabled
//                                (works even while muted) and doesn't touch
//                                the alert cooldown, since it's an explicit
//                                one-off request, not a motion alert
//   /status                    - list every enabled camera's on/off state
// /on, /off, and /status require TelegramUser::canCommand; /snap requires
// the separate TelegramUser::canSnap instead - a user can have either, both,
// or neither (see telegram_users.h for why they're independent). Each /on
// or /off toggle is persisted via NVS so it survives a reboot. The reply
// (and, for /snap, the photo) goes back to whichever chat sent the command.
// A chat ID that doesn't belong to any Telegram user, or belongs to one
// with neither permission, is ignored outright; one with only one of the
// two gets a plain "not authorized" reply if they try a command outside
// their scope, rather than being silently ignored like a stranger would be.
// Call
// periodically from loop() (e.g. every TELEGRAM_COMMAND_POLL_MS) - each
// call is one short, non-blocking-long-poll HTTPS round trip (plus, for a
// /snap, the snapshot fetch/upload itself), safe to call often.
void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras);
