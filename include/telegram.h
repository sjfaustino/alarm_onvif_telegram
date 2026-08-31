#pragma once
#include <Arduino.h>
#include <vector> // explicit, not chained - see camera_serialize.h's comment; recentUnknownChats' return type
#include "config.h"
#include "camera.h"

// Sends cfg.snapshotBurstCount snapshot(s), captioned "<camera name> -
// <UTC timestamp>" (plus "(n/N)"), to every subscribed user, subject to
// cfg's alert cooldown. Safe to call on every motion event.
void triggerMotionAlert(const CameraConfig& cfg, CameraState& st);

// Tamper/signal-loss alerts, gated by the same alertsEnabled/cooldown
// subscribed-recipients rules triggerMotionAlert uses (one shared
// alertCooldownMs per camera - there's no separate config surface for a
// per-event-type cooldown). Safe to call on every event.
//
// triggerTamperAlert attempts a single snapshot (not the configured
// burst) since physical tampering usually still has *some* usable video
// at that instant, unlike an outright signal loss - falls back to a
// text-only message if the fetch fails or no snapshot URI is known yet,
// rather than staying silent just because a photo isn't available.
void triggerTamperAlert(const CameraConfig& cfg, CameraState& st);

// Always text-only, never attempts a snapshot - by definition the video
// feed is the thing that's gone.
void triggerSignalLossAlert(const CameraConfig& cfg, CameraState& st);

// Broadcasts an OFFLINE/back-ONLINE notice on a lastContactMs/offlineThresholdMs
// state transition. Cheap enough to call every cameraTaskFn loop iteration.
void checkCameraOnlineStatus(const CameraConfig& cfg, CameraState& st);

// Broadcasts an alert if this camera hasn't seen a real motion event
// (CameraState::lastMotionMs, updated independently of mute/cooldown/quiet
// hours) in over cfg.motionWatchdogHours - a no-op if that's 0 (off,
// default). Re-arms (won't alert again) once motion resumes. Cheap enough
// to call every cameraTaskFn loop iteration, same as checkCameraOnlineStatus.
void checkMotionWatchdog(const CameraConfig& cfg, CameraState& st);

// Once triggerMotionAlert's cooldown (started by a real, non-quiet-hours
// snapshot send) ends, sends one summary text of how many further motion
// events landed during it, if any - "did motion continue after the photo,
// or was it a one-off" without a photo per event. No-op most calls (no
// digest pending, or cooldown still running). Cheap enough to call every
// cameraTaskFn loop iteration, same as checkCameraOnlineStatus/
// checkMotionWatchdog above.
void checkPendingMotionDigest(const CameraConfig& cfg, CameraState& st);

// Broadcasts an alert if this camera has been responding (see
// checkCameraOnlineStatus - not OFFLINE) but hasn't held a working
// subscription in over cfg.offlineThresholdMs, so it can't actually report
// any motion/tamper/signal-loss event - the case checkCameraOnlineStatus's
// own lastContactMs can't catch on its own, since a camera answering every
// call with a SOAP fault keeps lastContactMs fresh forever without ever
// subscribing (see cameraSoapCall's own comment, camera.cpp). No-op while
// isOffline is already true - that's a distinct, already-alerted condition.
// Re-arms once subscribed again. Call this AFTER checkCameraOnlineStatus
// each cameraTaskFn loop iteration, so st.isOffline is current.
void checkSubscriptionHealth(const CameraConfig& cfg, CameraState& st);

// Captures exactly one snapshot and stores it via pushCameraSnapshot (SD
// if active, RAM ring otherwise) - never sent to Telegram, no recipients,
// no cooldown interaction. No-op if st.snapshotUri hasn't resolved yet.
// Called from cameraTaskFn on cfg.timelapseIntervalMin's own interval,
// independent of motion/alerts entirely.
void triggerTimelapseCapture(const CameraConfig& cfg, CameraState& st);

// Sends text to every user with systemMessages enabled. Returns false if
// no user has it enabled, or every send failed.
bool sendTelegramMessage(const String& text);

// True once TELEGRAM_ROOT_CA holds a real certificate - false means every
// send will fail TLS verification.
bool telegramCAConfigured();

// Reads camera `index`'s persisted alerts-enabled flag from NVS (default
// true). Call once per camera at boot, before spawning its task.
bool loadAlertEnabledPref(size_t index);

// One chat ID that recently messaged the bot without matching any
// configured TelegramUser - see recentUnknownChats' own comment.
struct UnknownChatSighting {
  int64_t chatId = 0;
  unsigned long lastSeenMs = 0;
};

// How many distinct chat IDs recentUnknownChats() tracks - shared with the
// Users page's own hint text, so both agree on the number.
static const size_t UNKNOWN_CHAT_TRACK_MAX = 5;

// Up to UNKNOWN_CHAT_TRACK_MAX most recently seen chat IDs that messaged
// the bot without matching any configured TelegramUser,
// newest first - a convenience for the Users page so adding a new user is
// copy-paste from here instead of a side trip to @userinfobot or the raw
// getUpdates URL. RAM-only, doesn't grow, and isn't a security log - see
// telegram.cpp's own comment on the tracking table itself.
std::vector<UnknownChatSighting> recentUnknownChats();

// Turns every currently-enabled camera's alerts on/off at once, with an
// optional timer - the shared implementation behind /on all, /off all
// (pollTelegramCommands, below) and the Cameras page's own "Mute all"/
// "Unmute all" buttons (webserver.cpp). durationText follows /on|/off's
// own duration-token syntax ("" = permanent, minutes, or "HH:MM" - see
// parseDurationToken, telegram_parse.h); viaWho is a short label for the
// Serial/Activity log ("Telegram (name)", "the dashboard"). Returns a
// plain-text result for the caller to show however it likes - success, or
// the specific reason nothing happened (no enabled cameras, or an
// unparseable duration).
String setAllCamerasAlertState(const CameraConfig cameras[], CameraState states[], size_t numCameras,
                                bool turnOn, const String& durationText, const String& viaWho);

// Polls getUpdates and applies commands, matched by case-insensitive
// camera-name prefix ("/on D01" matches "D01-FDir"; an ambiguous prefix
// lists the matches instead of applying anything) - or the literal word
// "all" in place of a name/prefix, which applies to every enabled camera
// at once ("/off all 30" mutes everything for 30 minutes; "/snap all"
// fetches a fresh photo from every camera). A real camera named starting
// with "all" would be unreachable by its own prefix as a result - an
// accepted, extremely narrow trade-off, not a bug.
//   /on|/off|/snap with no target at all - shows a tappable inline-
//                              keyboard camera picker instead (one button
//                              per enabled camera plus "All"); permanent
//                              on/off/snap only, no duration timer via
//                              buttons. See handleTelegramCallbackQuery
//                              (telegram.cpp) for how a tap is handled.
//   /on|/off <name/prefix|all> [duration] - resume/mute alerts
//                              (subscription stays up either way).
//                              Optional trailing duration schedules an
//                              automatic revert back to the opposite
//                              state - see parseDurationToken
//                              (telegram_parse.h) for exactly what it
//                              accepts (plain minutes, or a 24h "HH:MM"
//                              clock time). Omitted entirely means
//                              permanent, the original behavior.
//   /snap <name/prefix|all>  - fresh snapshot now, ignoring mute/cooldown
//   /status                  - list every enabled camera's on/off state
//   /uptime                  - board uptime
//   /health                  - free heap/PSRAM, NVS usage, WiFi signal, SD storage status
//   /log [N]                 - the N most recent Activity log entries (default 10)
//   /reset                   - reboot the board immediately
//   /help                    - this command list, plus the sender's own permissions
// /on, /off, /status, /uptime, /health, /log require canCommand; /snap requires
// canSnap; /reset requires canReset (off by default, even for the seeded
// Admin user - see TelegramUser::canReset); /help requires none of the above.
void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras);

// Flips alertsEnabled back for any camera whose timed /on or /off (see
// pollTelegramCommands) has reached its scheduled revert time - call once
// per loop() tick (main.cpp), same cadence as pollTelegramCommands itself.
// Cheap when nothing's due: just a millis() comparison per camera.
void checkScheduledAlertReverts(const CameraConfig cameras[], CameraState states[], size_t numCameras);
