#pragma once
#include <Arduino.h>
#include <functional>
#include <vector> // explicit, not chained - see camera_serialize.h's comment; recentUnknownChats' return type
#include "config.h"
#include "camera.h"
#include "telegram_users.h" // TelegramLang
#include "telegram_i18n.h" // MotionDetectionKind

// Sends cfg.snapshotBurstCount snapshots to every subscribed user, subject to
// the camera's cooldown. Safe to call on every motion event.
//
// isPetEvent: DogCatDetect event - same gating, pet wording, and text-only if
// cfg.petAlertsTextOnly. kind: marks the caption PERSON/VEHICLE so phone-side
// automations (MacroDroid/Tasker) can pick a sound per detection type.
void triggerMotionAlert(const CameraConfig& cfg, CameraState& st, bool isPetEvent = false,
                         MotionDetectionKind kind = MotionDetectionKind::Generic);

// Tamper/signal-loss alerts share the motion alert's mute and cooldown rules.
// Tamper sends one snapshot (falling back to text); signal loss is text-only,
// since the video is what's gone.
void triggerTamperAlert(const CameraConfig& cfg, CameraState& st);

void triggerSignalLossAlert(const CameraConfig& cfg, CameraState& st);

// OFFLINE/back-ONLINE notice on a state transition. Called every task loop.
void checkCameraOnlineStatus(const CameraConfig& cfg, CameraState& st);

// Alerts if no real motion in cfg.motionWatchdogHours (0 = off); re-arms when
// motion resumes.
void checkMotionWatchdog(const CameraConfig& cfg, CameraState& st);

// After the post-photo cooldown ends, sends one summary of the motion counted
// during it, if any.
void checkPendingMotionDigest(const CameraConfig& cfg, CameraState& st);

// One extra summary when different cameras alert within
// MULTI_CAMERA_DIGEST_WINDOW_MS (storm, wind, a shared false positive).
// Per-camera alerts are unaffected. Global state; any camera task may call it.
void checkMultiCameraAlertDigest();

// Alerts when a camera answers (so isn't OFFLINE) but hasn't held a
// subscription for cfg.offlineThresholdMs - e.g. every call returns a SOAP
// fault. Call after checkCameraOnlineStatus.
void checkSubscriptionHealth(const CameraConfig& cfg, CameraState& st);

// Stores one snapshot (SD or RAM ring); never sent to Telegram.
void triggerTimelapseCapture(const CameraConfig& cfg, CameraState& st);

// Dashboard "Send test alert": one fresh snapshot to this camera's
// subscribers, captioned as a test, without touching cooldown/digest state.
// Blocks for seconds, so run it off the web server task. kind/isPetEvent pick
// the caption variant so phone automations can be tested.
bool sendTestAlert(const CameraConfig& cfg, CameraState& st, String& outDetail,
                    MotionDetectionKind kind = MotionDetectionKind::Generic, bool isPetEvent = false);

// True if a Telegram send is in flight or queued. Lets memory-heavy background
// jobs (discovery, Test all) wait instead of overlapping a photo send's
// buffers.
bool telegramSendInProgress();

// Sends to every user with systemMessages enabled, composed per recipient in
// their language. False if nobody is eligible or every send failed.
bool sendTelegramMessage(std::function<String(TelegramLang)> compose);

// Single pre-composed message, used by the retry queue.
bool sendTelegramMessageToChatId(const String& chatId, const String& text);

bool telegramCAConfigured();

// Persisted /on /off state for camera `index` (default true).
bool loadAlertEnabledPref(size_t index);

// Chat that messaged the bot but isn't a configured user.
struct UnknownChatSighting {
  int64_t chatId = 0;
  unsigned long lastSeenMs = 0;
};

static const size_t UNKNOWN_CHAT_TRACK_MAX = 5;

// Newest-first unknown chat IDs, so the Users page can offer them for
// copy-paste. RAM-only; not a security log.
std::vector<UnknownChatSighting> recentUnknownChats();

// /on all, /off all and the dashboard's Mute/Unmute all. durationText uses the
// /on|/off syntax ("" = permanent, minutes, or "HH:MM"). Returns the result
// text in `lang` (the dashboard always passes English).
String setAllCamerasAlertState(const CameraConfig cameras[], CameraState states[], size_t numCameras,
                                bool turnOn, const String& durationText, const String& viaWho,
                                TelegramLang lang);

// Polls getUpdates and runs commands. Cameras are matched by case-insensitive
// name prefix (ambiguous prefixes list the matches) or "all" (so a camera
// named "all..." is unreachable by prefix - accepted).
//   /on|/off|/snap              - no target: inline-keyboard camera picker
//   /on|/off <name|all> [dur]   - resume/mute; optional duration reverts later
//                                 (minutes or "HH:MM", see parseDurationToken)
//   /snap <name|all>            - fresh snapshot, ignoring mute/cooldown
//   /status, /uptime, /health, /log [N], /reset, /lang [en|pt], /help
// Permissions: canCommand for on/off/status/uptime/health/log, canSnap for
// /snap, canReset for /reset (off by default); /help and /lang need none.
void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras);

// Reverts timed /on or /off that are due. Every loop() tick; cheap.
void checkScheduledAlertReverts(const CameraConfig cameras[], CameraState states[], size_t numCameras);

// Sends per-camera detection counts and resets them, so call only every
// DAILY_DIGEST_INTERVAL_MS. Sends nothing if all counts are zero.
void checkDailyActivityDigest(const CameraConfig cameras[], CameraState states[], size_t numCameras);

// millis() due time as local "HH:MM", or "" if the clock isn't synced (a time
// from an unsynced clock would mislead).
String formatLocalClockTime(unsigned long dueMs);
