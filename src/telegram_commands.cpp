#include "telegram.h"
#include "telegram_internal.h"
#include "config.h"
#include "telegram_ca.h"
#include "telegram_users.h"
#include "telegram_i18n.h"
#include "telegram_parse.h"
#include "format_utils.h"
#include "event_log_store.h"
#include "snapshot_history.h"
#include "sd_store.h"
#include "config_backup.h"
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include <cstring>
#include <vector>
#include <algorithm>

// ============================================================
// Remote on/off control (Telegram commands)
//
// pollTelegramCommands() runs periodically from loop() (short getUpdates,
// not long-poll). lastUpdateId is persisted in NVS - it used not to be, on
// the theory that redelivering a couple of already-applied idempotent
// commands after a reboot is harmless. /reset broke that: redelivering it
// after the reboot it caused re-executes /reset again, forever - a real
// infinite reboot loop hit the first time /reset was used.
//
// Persisting on every update closed that loop but opened a smaller one:
// Telegram delivers every inbound message regardless of sender, so an
// unauthenticated flood would force an NVS write per message. Persisting
// once per poll instead (below) bounds that while keeping the original
// redelivery assumption for everything except /reset.
//
// /reset can't wait for that end-of-poll persist - ESP.restart() never
// returns - so it's persisted inside handleTelegramCommand's own /reset
// branch, immediately before the restart, rather than pollTelegramCommands
// pre-guessing which commands are "dangerous" (an earlier version did
// exactly that, checking canReset/the command text in two places that
// drifted out of sync with each other).
// ============================================================

static const char* TELEGRAM_STATE_NAMESPACE = "tgstate";
static const char* TELEGRAM_STATE_KEY_LAST_UPDATE_ID = "lastUpdateId";

static long loadLastUpdateId() {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(TELEGRAM_STATE_NAMESPACE, false);
  long id = prefs.getLong(TELEGRAM_STATE_KEY_LAST_UPDATE_ID, 0);
  prefs.end();
  return id;
}

static void saveLastUpdateId(long id) {
  Preferences prefs;
  if (prefs.begin(TELEGRAM_STATE_NAMESPACE, false)) {
    prefs.putLong(TELEGRAM_STATE_KEY_LAST_UPDATE_ID, id);
    prefs.end();
  }
}

// ============================================================
// Recent unrecognized chat IDs - RAM-only, small fixed table (not NVS/a
// growable log): purely a convenience so adding a new Telegram user can be
// copy-paste from the Users page instead of a side trip to @userinfobot or
// the raw getUpdates URL, for whoever most recently actually messaged this
// bot. Not a security log - deliberately doesn't grow, persist, or record
// anything about WHO/WHAT was sent, just "this chat ID messaged the bot
// recently" for the one specific case (!sender in pollTelegramCommands)
// where the sender isn't a configured user at all.
// ============================================================

// Internal-only add-on to the header's own UnknownChatSighting - `used`
// marks a still-empty slot, never exposed outside this file.
struct UnknownChatSlot {
  int64_t chatId = 0;
  unsigned long lastSeenMs = 0;
  bool used = false;
};
static UnknownChatSlot g_unknownChats[UNKNOWN_CHAT_TRACK_MAX];
static SemaphoreHandle_t g_unknownChatsMutex = xSemaphoreCreateMutex();

// Same exact-match-or-least-recently-seen-eviction shape as webserver.cpp's
// RateLimitMiddleware::findOrCreate - unrelated tables, same small-fixed-
// size-tracking problem.
static void recordUnknownChat(int64_t chatId) {
  xSemaphoreTake(g_unknownChatsMutex, portMAX_DELAY);
  UnknownChatSlot* slot = nullptr;
  for (auto& e : g_unknownChats) {
    if (e.used && e.chatId == chatId) { slot = &e; break; }
  }
  if (!slot) {
    slot = &g_unknownChats[0];
    for (auto& e : g_unknownChats) {
      if (!e.used) { slot = &e; break; }
      if (e.lastSeenMs < slot->lastSeenMs) slot = &e;
    }
  }
  slot->chatId = chatId;
  slot->lastSeenMs = millis();
  slot->used = true;
  xSemaphoreGive(g_unknownChatsMutex);
}

std::vector<UnknownChatSighting> recentUnknownChats() {
  std::vector<UnknownChatSighting> result;
  xSemaphoreTake(g_unknownChatsMutex, portMAX_DELAY);
  for (auto& e : g_unknownChats) {
    if (e.used) result.push_back({e.chatId, e.lastSeenMs});
  }
  xSemaphoreGive(g_unknownChatsMutex);
  std::sort(result.begin(), result.end(),
            [](const UnknownChatSighting& a, const UnknownChatSighting& b) { return a.lastSeenMs > b.lastSeenMs; });
  return result;
}

// ============================================================
// Per-user Telegram command rate limiting - see TelegramUser::
// maxCommandsPerMinute's own comment (telegram_users.h) for why this is
// enforced as a minimum gap between commands rather than a true rolling-
// window count: O(1) state per user (just a last-command timestamp), no
// ring buffer. RAM-only, small fixed table, same exact-match-or-least-
// recently-seen-eviction shape as g_unknownChats above and webserver.cpp's
// RateLimitMiddleware::findOrCreate (unrelated tables, same small-fixed-
// size-tracking problem).
// ============================================================

static const size_t COMMAND_RATE_TRACK_MAX = 16; // generous for any realistic configured-user count

struct CommandRateSlot {
  String chatId;
  unsigned long lastCommandMs = 0;
  bool used = false;
};
static CommandRateSlot g_commandRateTable[COMMAND_RATE_TRACK_MAX];
static SemaphoreHandle_t g_commandRateMutex = xSemaphoreCreateMutex();

// Only meaningful when maxCommandsPerMinute > 0 - callers gate on that
// themselves rather than this function treating 0 as "unlimited", so it
// stays a plain "check and record" primitive. Returns true (and records
// this command's timestamp) if chatId may send a command right now; a
// chat seen for the very first time is always allowed.
static bool allowTelegramCommand(const String& chatId, uint16_t maxCommandsPerMinute) {
  unsigned long minIntervalMs = 60000UL / maxCommandsPerMinute;
  unsigned long now = millis();

  xSemaphoreTake(g_commandRateMutex, portMAX_DELAY);
  CommandRateSlot* slot = nullptr;
  for (auto& e : g_commandRateTable) {
    if (e.used && e.chatId == chatId) { slot = &e; break; }
  }
  if (!slot) {
    slot = &g_commandRateTable[0];
    for (auto& e : g_commandRateTable) {
      if (!e.used) { slot = &e; break; }
      if (e.lastCommandMs < slot->lastCommandMs) slot = &e;
    }
  }

  // Same unsigned-subtraction wraparound-safe shape as beginCameraAlert's
  // own cooldown check above - a never-before-seen slot has nothing to
  // compare against, so it's always allowed.
  bool allowed = !slot->used || (now - slot->lastCommandMs) >= minIntervalMs;
  if (allowed) {
    slot->chatId = chatId;
    slot->lastCommandMs = now;
    slot->used = true;
  }
  xSemaphoreGive(g_commandRateMutex);
  return allowed;
}

static const char* ALERT_PREF_NAMESPACE = "camctl";

bool loadAlertEnabledPref(size_t index) {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(ALERT_PREF_NAMESPACE, false);
  char key[8];
  snprintf(key, sizeof(key), "c%u", (unsigned)index);
  bool enabled = prefs.getBool(key, true); // default ON if never set
  prefs.end();
  return enabled;
}

static void saveAlertEnabledPref(size_t index, bool enabled) {
  Preferences prefs;
  prefs.begin(ALERT_PREF_NAMESPACE, false);
  char key[8];
  snprintf(key, sizeof(key), "c%u", (unsigned)index);
  prefs.putBool(key, enabled);
  prefs.end();
}

// Fetches a fresh snapshot from cfg/st right now and sends it only to
// chatId (whoever asked) - unlike triggerMotionAlert, this is an explicit
// one-off request, not a motion alert, so it ignores st.alertsEnabled and
// doesn't touch st.lastAlert/hasAlerted or spend the alert cooldown.
static void sendOnDemandSnapshot(const CameraConfig& cfg, CameraState& st, const String& chatId, TelegramLang lang) {
  // This runs on loop()'s task, snapshotUri is written by the camera's own
  // task - cross-task read, needs CameraStateLock. See CameraState::stateMutex.
  bool hasSnapshotUri;
  { CameraStateLock lock(st); hasSnapshotUri = st.snapshotUri.length() > 0; }
  if (!hasSnapshotUri) {
    sendTelegramMessageTo(chatId, trNoSnapshotUriYet(lang, cfg.name));
    return;
  }

  size_t jpgLen = 0;
  uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
  if (!jpg) {
    sendTelegramMessageTo(chatId, trSnapshotFetchFailed(lang, cfg.name));
    return;
  }

  String caption = cfg.name + " - " + nowTimestampString();
  if (!sendTelegramPhotoWithRetry(jpg, jpgLen, caption, chatId)) {
    Serial.printf("[%s] On-demand snapshot send to chat %s failed.\n", cfg.name.c_str(), chatId.c_str());
  }
  pushCameraSnapshot(cfg, st, jpg, jpgLen, SnapshotSource::Manual); // takes ownership
}

// Result of resolveAlertTimer below - shared by the single-camera and
// all-cameras /on//off paths in handleTelegramCommand/handleAllCamerasCommand
// so the duration-parsing logic (and its error handling) can't drift
// between the two copies the way independently-duplicated parsing already
// caused a real bug once in this project (the /reset reboot loop).
struct AlertTimer {
  bool ok = true;               // false only if durationText was non-empty and unparseable
  bool hasTimer = false;        // true if durationText was non-empty and DID parse
  unsigned long revertDueMs = 0; // valid only if hasTimer
  String suffix;                 // " (auto ON in 1h30m)" etc, "" if !hasTimer - appended to the reply
  String errorMsg;               // set only if !ok - what to reply with
};

// durationText is parsed.durationText ("" means no timer, permanent
// on/off - the original behavior). Resolving "HH:MM" needs the actual
// current local time, which parseDurationToken (telegram_parse.h)
// deliberately doesn't read for itself - see its own comment.
static AlertTimer resolveAlertTimer(const String& durationText, bool turnOn, TelegramLang lang) {
  AlertTimer result;
  if (durationText.length() == 0) return result;

  time_t now; time(&now);
  struct tm nowLocal; localtime_r(&now, &nowLocal);
  ParsedDuration dur = parseDurationToken(durationText, nowLocal);
  if (!dur.ok) {
    result.ok = false;
    result.errorMsg = trDurationParseError(lang, durationText, MAX_DURATION_MINUTES);
    return result;
  }
  result.hasTimer = true;
  result.revertDueMs = millis() + dur.secondsFromNow * 1000UL;
  result.suffix = trTimerSuffix(lang, turnOn, dur.secondsFromNow * 1000UL);
  return result;
}

// Persists a user's own /lang switch (text command or the inline-keyboard
// picker tap both funnel through here) and replies with confirmation IN
// THE NEW language - the whole point of switching is to see the very next
// message in it, not the one that's about to become stale. `sender` is a
// const& into loadTelegramUsers()'s own temporary from this poll
// (pollTelegramCommands), so this never mutates it in place - a fresh
// TelegramUser copy is written back to NVS by name (the unique key),
// which the NEXT poll's loadTelegramUsers() picks up.
static void applyLanguageChange(const TelegramUser& sender, TelegramLang newLang) {
  TelegramUser updated = sender;
  updated.language = newLang;
  if (!updateTelegramUser(sender.name, updated)) {
    Serial.printf("[Telegram] Failed to persist language change for user \"%s\".\n", sender.name.c_str());
    sendTelegramMessageTo(sender.chatId, trLanguageChangeFailed(sender.language));
    return;
  }
  Serial.printf("[Telegram] User \"%s\" changed Telegram language.\n", sender.name.c_str());
  logEvent(sender.name + ": changed Telegram language");
  sendTelegramMessageTo(sender.chatId, trLanguageChanged(newLang));
}

// Sets one camera's alerts on/off, persists it (NVS), logs it, and
// replies with confirmation - the single-camera state-mutation tail
// shared by the text-command path (/on|/off <camera> [duration], see
// handleTelegramCommand's own tail below) and the inline-keyboard button
// path (handleTelegramCallbackQuery, always a default-constructed
// AlertTimer - permanent, no duration support via buttons). Sharing this
// one implementation is what keeps the button path from silently
// diverging from the text-command path on reboot-persistence (a change
// here or a forgotten saveAlertEnabledPref call would otherwise only be
// caught in one of the two places).
static void applyOnOffToCamera(const CameraConfig& cfg, CameraState& st, size_t index, bool turnOn,
                                const AlertTimer& timer, const String& viaWho, const String& replyChatId,
                                TelegramLang lang) {
  {
    CameraStateLock lock(st); // read cross-task by camera.cpp/webserver.cpp
    st.alertsEnabled = turnOn;
    // A plain (no-timer) /on or /off cancels whatever timer was pending
    // before - issuing a new command always replaces the old schedule,
    // never stacks with it.
    st.scheduledRevertDueMs = timer.hasTimer ? timer.revertDueMs : 0;
    st.scheduledRevertToOn = !turnOn;
  }
  saveAlertEnabledPref(index, turnOn);
  Serial.printf("[%s] Alerts turned %s via Telegram by user \"%s\"%s.\n", cfg.name.c_str(),
                turnOn ? "ON" : "OFF", viaWho.c_str(), timer.hasTimer ? " (timed)" : "");
  logEvent(cfg.name + " alerts: " + (turnOn ? "ON" : "OFF") + " via " + viaWho + timer.suffix);
  sendTelegramMessageTo(replyChatId, trAlertsState(lang, cfg.name, turnOn, timer.suffix));
}

// Shared by /on all, /off all [duration] (via handleAllCamerasCommand
// below) and the Cameras page's own "Mute all"/"Unmute all" buttons
// (webserver.cpp) - the actual state-mutation logic can't drift between
// the two front-ends the way applyOnOffToCamera already prevents for the
// single-camera case. durationText is parsed the same way /on's own timer
// token is ("" = permanent, a plain number of minutes, or "HH:MM" - see
// parseDurationToken, telegram_parse.h); viaWho is a short human label for
// the Serial/Activity log ("Telegram (name)", "the dashboard"). Returns a
// plain-text result - success or the specific reason nothing happened
// (no enabled cameras, or an unparseable duration) - for the caller to
// relay however it likes (a Telegram reply, a web banner).
String setAllCamerasAlertState(const CameraConfig cameras[], CameraState states[], size_t numCameras,
                                bool turnOn, const String& durationText, const String& viaWho,
                                TelegramLang lang) {
  std::vector<size_t> targets;
  for (size_t i = 0; i < numCameras; i++) {
    if (cameras[i].enabled) targets.push_back(i);
  }
  if (targets.empty()) return trNoEnabledCameras(lang);

  AlertTimer timer = resolveAlertTimer(durationText, turnOn, lang);
  if (!timer.ok) return timer.errorMsg;

  for (size_t i : targets) {
    { CameraStateLock lock(states[i]); states[i].alertsEnabled = turnOn;
      states[i].scheduledRevertDueMs = timer.hasTimer ? timer.revertDueMs : 0;
      states[i].scheduledRevertToOn = !turnOn; }
    saveAlertEnabledPref(i, turnOn);
  }
  Serial.printf("Alerts turned %s for all %u camera(s) via %s%s.\n", turnOn ? "ON" : "OFF",
                (unsigned)targets.size(), viaWho.c_str(), timer.hasTimer ? " (timed)" : "");
  logEvent("All cameras alerts: " + String(turnOn ? "ON" : "OFF") + " via " + viaWho + timer.suffix);
  return trAlertsState(lang, trAllCamerasSubject(lang, targets.size()), turnOn, timer.suffix);
}

// Applies /on all, /off all [duration], or /snap all to every currently-
// enabled camera - see pollTelegramCommands' (telegram.h) comment on the
// "all" keyword for the (extremely narrow) trade-off it makes against a
// real camera named starting with "all". Caller (handleTelegramCommand)
// has already matched parsed.cameraName == "all" case-insensitively
// before reaching here.
static void handleAllCamerasCommand(const TelegramUser& sender, const ParsedTelegramCommand& parsed,
                                     const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  if (parsed.command == TelegramCommand::Snap) {
    std::vector<size_t> targets;
    for (size_t i = 0; i < numCameras; i++) {
      if (cameras[i].enabled) targets.push_back(i);
    }
    if (targets.empty()) {
      sendTelegramMessageTo(sender.chatId, trNoEnabledCameras(sender.language));
      return;
    }
    Serial.printf("[Telegram] On-demand snapshot of all %u camera(s) requested by user \"%s\".\n",
                  (unsigned)targets.size(), sender.name.c_str());
    for (size_t i : targets) {
      sendOnDemandSnapshot(cameras[i], states[i], sender.chatId, sender.language);
      // A fetch+send per camera, synchronously, all within this one
      // loop() tick - main.cpp's loop() only resets the task watchdog at
      // its own top, so enough slow/unresponsive cameras in one "/snap
      // all" could otherwise add up toward WATCHDOG_TIMEOUT_MS (90s) and
      // panic-reboot the board over a Telegram command. Same reasoning,
      // same fix, as camera_store.cpp's restoreMissingCamerasFromSeed().
      esp_task_wdt_reset();
    }
    return;
  }

  bool turnOn = (parsed.command == TelegramCommand::On);
  String result = setAllCamerasAlertState(cameras, states, numCameras, turnOn, parsed.durationText,
                                           "Telegram (" + sender.name + ")", sender.language);
  sendTelegramMessageTo(sender.chatId, result);
}

// Sent when /on, /off, or /snap arrives with no camera name at all (see
// parseTelegramCommand's own comment on the bare-command case) - one
// button per enabled camera plus "All", each carrying
// "<verb>|<cameraNameOrAll>" as its callback_data for
// handleTelegramCallbackQuery (below) to act on when tapped. No
// duration-timer support via buttons - tap-to-toggle/snap only, permanent
// on/off.
static void sendCameraPickerKeyboard(const TelegramUser& sender, TelegramCommand command,
                                      const CameraConfig cameras[], size_t numCameras) {
  String verb = commandDisplayName(command).substring(1); // "on"/"off"/"snap" - drop the leading "/"

  std::vector<std::pair<String, String>> buttons;
  for (size_t i = 0; i < numCameras; i++) {
    if (!cameras[i].enabled) continue;
    buttons.push_back({cameras[i].name, verb + "|" + cameras[i].name});
  }
  if (buttons.empty()) {
    sendTelegramMessageTo(sender.chatId, trNoCamerasToChoose(sender.language));
    return;
  }
  // Label is translated; the "all" callback_data token itself must not be -
  // it's a protocol identifier handleTelegramCallbackQuery matches
  // case-insensitively, not display text.
  buttons.push_back({trAllButtonLabel(sender.language), verb + "|all"});

  size_t skipped = 0;
  sendTelegramKeyboardTo(sender.chatId, trCameraPickerPrompt(sender.language, commandDisplayName(command)),
                          buttons, &skipped);
  if (skipped > 0) {
    // Not expected to trigger with this project's camera names (see
    // sendTelegramKeyboardTo's own comment) - but if it ever does, the
    // camera(s) missing from the keyboard above shouldn't be a silent gap
    // only visible in the Serial log.
    sendTelegramMessageTo(sender.chatId,
                           trCallbackDataTooLong(sender.language, skipped, commandDisplayName(command)));
  }
}

// /restore's own pending-arm state - a bare "/restore" (see the
// TelegramCommand::Restore case below) sets this so the NEXT document
// upload from the SAME chat, within RESTORE_PENDING_WINDOW_MS
// (config.h), is treated as the file to restore from - see
// handleTelegramDocument's own comment for the full two-step (or
// one-step, caption="/restore") design. Single-slot, not a per-chat map -
// same "one at a time, not a queue" simplicity as this project's
// BackgroundJob<T> elsewhere; only one admin is ever expected to be
// mid-restore at once, and a second /restore from a different chat while
// one is already pending simply replaces it. Same-task-only:
// pollTelegramCommands (the sole caller of both handleTelegramCommand and
// handleTelegramDocument) only ever runs from loop()'s own task, so no
// lock is needed.
static String g_pendingRestoreChatId; // "" = none armed
static unsigned long g_pendingRestoreExpiresAtMs = 0;

// lastUpdateId is this poll's running highest update_id, already advanced
// past `text`'s own update - passed through so the Reset case can persist
// it immediately, before ESP.restart() (see this section's top comment).
//
// text is parsed exactly once, by parseTelegramCommand (telegram_parse.h) -
// command identity, permission, and camera name are decided there and
// used as-is below, not re-derived here. The switch below has no default
// case (-Werror=switch, telegram_parse's library.json) so a new
// TelegramCommand added without a case here is a build failure, not a
// silent "unrecognized, ignored".
static void handleTelegramCommand(const TelegramUser& sender, const String& text, const CameraConfig cameras[],
                                   CameraState states[], size_t numCameras, long lastUpdateId) {
  Serial.printf("[Telegram] Command from user \"%s\": \"%s\"\n", sender.name.c_str(), text.c_str());

  ParsedTelegramCommand parsed = parseTelegramCommand(text);

  bool authorized = false;
  switch (parsed.requiredPermission) {
    case TelegramCommandPermission::Command: authorized = sender.canCommand; break;
    case TelegramCommandPermission::Snap:    authorized = sender.canSnap;    break;
    case TelegramCommandPermission::Reset:   authorized = sender.canReset;   break;
    case TelegramCommandPermission::Backup:  authorized = sender.canBackup;  break;
    case TelegramCommandPermission::Restore: authorized = sender.canRestore; break;
    case TelegramCommandPermission::Unknown: authorized = false;             break;
  }
  // Logged here for every command now, including /status/uptime/reset -
  // an earlier version of this check only logged rejected /on//off/snap
  // attempts, silently replying with no server-side trace for the others.
  // Calling that out explicitly since it wasn't when this unification
  // first landed.
  if (parsed.requiredPermission != TelegramCommandPermission::Unknown && !authorized) {
    String name = commandDisplayName(parsed.command);
    Serial.printf("[Telegram] User \"%s\" not authorized for %s.\n", sender.name.c_str(), name.c_str());
    sendTelegramMessageTo(sender.chatId, trNotAuthorized(sender.language, name));
    return;
  }

  if (sender.maxCommandsPerMinute > 0 && !allowTelegramCommand(sender.chatId, sender.maxCommandsPerMinute)) {
    Serial.printf("[Telegram] User \"%s\" rate-limited (max %u command(s)/minute).\n", sender.name.c_str(),
                  (unsigned)sender.maxCommandsPerMinute);
    sendTelegramMessageTo(sender.chatId, trRateLimited(sender.language));
    return;
  }

  switch (parsed.command) {
    case TelegramCommand::Status: {
      // isOffline and the timer fields are written by other tasks
      // (camera.cpp's own task, and loop()'s task via handleTelegramCommand
      // /checkScheduledAlertReverts - this read runs on loop()'s task too,
      // but isOffline specifically crosses from the camera's own task, so
      // the whole group is read under one lock for simplicity rather than
      // splitting into a locked and an unlocked half. See
      // CameraState::stateMutex.
      String msg = trStatusHeader(sender.language) + "\n";
      for (size_t i = 0; i < numCameras; i++) {
        if (!cameras[i].enabled) continue;
        bool alertsEnabled, offline;
        unsigned long revertDueMs;
        bool revertToOn;
        size_t latencyCount = 0;
        unsigned long latencySum = 0;
        {
          CameraStateLock lock(states[i]);
          alertsEnabled = states[i].alertsEnabled;
          offline = states[i].isOffline;
          revertDueMs = states[i].scheduledRevertDueMs;
          revertToOn = states[i].scheduledRevertToOn;
          latencyCount = states[i].motionLatencyHistoryCount;
          for (size_t j = 0; j < latencyCount; j++) latencySum += states[i].motionLatencyHistory[j];
        }
        String timerSuffix;
        if (revertDueMs != 0 && (long)(millis() - revertDueMs) < 0) {
          timerSuffix = trTimerSuffix(sender.language, !revertToOn, revertDueMs - millis());
        }
        long avgLatencyMs = latencyCount > 0 ? (long)(latencySum / latencyCount) : -1;
        msg += trStatusCameraLine(sender.language, cameras[i].name, alertsEnabled, offline, timerSuffix,
                                   avgLatencyMs) + "\n";
      }
      Serial.printf("[Telegram] Replying to user \"%s\" with camera status.\n", sender.name.c_str());
      sendTelegramMessageTo(sender.chatId, msg);
      return;
    }

    case TelegramCommand::Uptime:
      Serial.printf("[Telegram] Replying to user \"%s\" with uptime.\n", sender.name.c_str());
      sendTelegramMessageTo(sender.chatId, trUptimeLine(sender.language, millis()));
      return;

    case TelegramCommand::Reset:
      Serial.printf("[Telegram] Reboot requested by user \"%s\" via /reset.\n", sender.name.c_str());
      // Persisted here, immediately before the irreversible action - see
      // this function's own top comment for why it's decided here and not
      // by pollTelegramCommands ahead of time.
      saveLastUpdateId(lastUpdateId);
      // Reply before restarting - ESP.restart() never returns, so this is
      // the last chance to confirm the command was actually received.
      sendTelegramMessageTo(sender.chatId, trRebootingNow(sender.language));
      delay(500); // let the TLS send above finish flushing before the reboot tears down WiFi
      // ESP.restart() doesn't wait for other FreeRTOS tasks to finish
      // whatever they're doing - if a camera task is mid-write to SD at
      // this exact moment, an uncoordinated reset could corrupt more than
      // just that one file (FAT isn't a journaling filesystem). See
      // waitForSdIdle's own comment (sd_store.h) for the full reasoning;
      // no-op if SD isn't active.
      waitForSdIdle();
      ESP.restart();
      return; // unreachable - ESP.restart() doesn't return - kept for a tidy switch

    case TelegramCommand::Backup: {
      Serial.printf("[Telegram] Config backup requested by user \"%s\" via /backup.\n", sender.name.c_str());
      String exportText = buildConfigExport();
      if (!sendTelegramDocumentBuffered(exportText, "camera-monitor-config.txt",
                                         trBackupCaption(sender.language), sender.chatId)) {
        Serial.printf("[Telegram] Backup send to user \"%s\" failed.\n", sender.name.c_str());
        sendTelegramMessageTo(sender.chatId, trBackupFailed(sender.language));
      }
      return;
    }

    case TelegramCommand::Restore:
      Serial.printf("[Telegram] Restore armed by user \"%s\" via /restore.\n", sender.name.c_str());
      g_pendingRestoreChatId = sender.chatId;
      g_pendingRestoreExpiresAtMs = millis() + RESTORE_PENDING_WINDOW_MS;
      sendTelegramMessageTo(sender.chatId, trRestorePrompt(sender.language));
      return;

    case TelegramCommand::On:
    case TelegramCommand::Off:
    case TelegramCommand::Snap:
      break; // handled below - shares the camera-name-matching logic

    case TelegramCommand::Help: {
      Serial.printf("[Telegram] Replying to user \"%s\" with /help.\n", sender.name.c_str());
      String msg = trHelpText(sender.language, (uint16_t)EVENT_LOG_CAPACITY, MAX_DURATION_MINUTES,
                               sender.canCommand, sender.canSnap, sender.canReset, sender.canBackup,
                               sender.canRestore);
      sendTelegramMessageTo(sender.chatId, msg);
      return;
    }

    case TelegramCommand::Health: {
      Serial.printf("[Telegram] Replying to user \"%s\" with /health.\n", sender.name.c_str());

      nvs_stats_t nvsStats;
      bool haveNvsStats = (nvs_get_stats(NULL, &nvsStats) == ESP_OK) && nvsStats.total_entries > 0;

      SdStatus sd = getSdStatus();
      String sdDetail;
      if (!sd.settingEnabled) {
        sdDetail = trSdDisabledDetail(sender.language);
      } else if (!sd.available) {
        sdDetail = trSdNotDetectedDetail(sender.language);
      } else {
        sdDetail = trSdDetail(sender.language, sd.cardTypeName, (double)sd.usedBytes / (1024.0 * 1024.0),
                               (double)sd.totalBytes / (1024.0 * 1024.0));
      }

      String msg = trHealthHeader(sender.language) + "\n";
      msg += trUptimeLine(sender.language, millis()) + "\n";
      msg += trFreeHeapLine(sender.language, ESP.getFreeHeap(), ESP.getMinFreeHeap()) + "\n";
      msg += trFreePsramLine(sender.language, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + "\n";
      if (haveNvsStats) {
        unsigned pct = (unsigned)((uint64_t)nvsStats.used_entries * 100 / nvsStats.total_entries);
        msg += trNvsUsageLine(sender.language, pct) + " (" + String((unsigned)nvsStats.used_entries) + " / " +
               String((unsigned)nvsStats.total_entries) + " entries)\n";
      }
      msg += trWifiSignalLine(sender.language, WiFi.RSSI()) + "\n";
      msg += trSdStorageLine(sender.language, sdDetail);

      sendTelegramMessageTo(sender.chatId, msg);
      return;
    }

    case TelegramCommand::Log: {
      long count = parsed.logCountText.length() > 0 ? parsed.logCountText.toInt() : 10;
      if (count < 1) count = 10; // garbage/zero/negative falls back to the default, not an error
      if (count > (long)EVENT_LOG_CAPACITY) count = (long)EVENT_LOG_CAPACITY;

      std::vector<EventLogEntry> events = recentEvents(); // oldest-first
      String msg = trLogHeader(sender.language) + "\n";
      if (events.empty()) {
        msg += trLogEmpty(sender.language);
      } else {
        // Newest first, same as webserver_activity.cpp's render - reverse
        // iterate, capped at `count`. Only the elapsed-time prefix is
        // translated - it->text is the Activity Log's own stored text,
        // which stays English everywhere (see lib/telegram_i18n.h's own
        // comment on why).
        long shown = 0;
        for (auto it = events.rbegin(); it != events.rend() && shown < count; ++it, ++shown) {
          msg += trElapsedSince(sender.language, it->ms, millis()) + " - " + it->text + "\n";
        }
      }
      Serial.printf("[Telegram] Replying to user \"%s\" with /log.\n", sender.name.c_str());
      sendTelegramMessageTo(sender.chatId, msg);
      return;
    }

    case TelegramCommand::Lang: {
      String argLower = parsed.langArgText;
      argLower.toLowerCase();
      if (argLower.length() == 0) {
        std::vector<std::pair<String, String>> buttons = {
          {"English", "lang|en"},
          {"Portugu\xC3\xAAs", "lang|pt"},
        };
        sendTelegramKeyboardTo(sender.chatId, trLanguagePickerPrompt(sender.language), buttons);
        return;
      }
      if (argLower == "en") {
        applyLanguageChange(sender, TelegramLang::English);
      } else if (argLower == "pt") {
        applyLanguageChange(sender, TelegramLang::Portuguese);
      } else {
        sendTelegramMessageTo(sender.chatId, trUnknownLanguageArg(sender.language, parsed.langArgText));
      }
      return;
    }

    case TelegramCommand::Unknown:
      Serial.println("[Telegram] Unrecognized command - ignored.");
      return;
  }

  // Bare /on, /off, or /snap (no target at all) - see parseTelegramCommand's
  // own comment on why this reaches here with an empty cameraName instead
  // of Unknown. Offer an inline-keyboard picker instead of falling through
  // to the "all"/prefix-matching logic below, which would otherwise wrongly
  // treat "" as matching every camera (matchCamerasByPrefix's
  // startsWith("") is unconditionally true).
  if (parsed.cameraName.length() == 0) {
    sendCameraPickerKeyboard(sender, parsed.command, cameras, numCameras);
    return;
  }

  // "all" (case-insensitive) is a special target meaning every enabled
  // camera at once, matched here rather than by matchCamerasByPrefix below
  // - see pollTelegramCommands' (telegram.h) comment on the trade-off that
  // makes for a real camera named starting with "all".
  String cameraNameLower = parsed.cameraName;
  cameraNameLower.toLowerCase();
  if (cameraNameLower == "all") {
    handleAllCamerasCommand(sender, parsed, cameras, states, numCameras);
    return;
  }

  // Matched by prefix ("D01" matches "D01-FDir") - ambiguous matches get
  // nothing applied and a reply listing what matched, rather than guessing.
  std::vector<size_t> matches = matchCamerasByPrefix(cameras, numCameras, parsed.cameraName);
  String verb = commandDisplayName(parsed.command).substring(1); // drop the leading "/" for mid-sentence use

  if (matches.size() > 1) {
    String list;
    for (size_t idx : matches) { if (list.length() > 0) list += ", "; list += cameras[idx].name; }
    Serial.printf("[Telegram] /%s target \"%s\" from user \"%s\" is ambiguous: %s\n", verb.c_str(),
                  parsed.cameraName.c_str(), sender.name.c_str(), list.c_str());
    sendTelegramMessageTo(sender.chatId, trAmbiguousCamera(sender.language, parsed.cameraName, list));
    return;
  }

  if (matches.empty()) {
    Serial.printf("[Telegram] /%s target not found or disabled: \"%s\" (user \"%s\")\n", verb.c_str(),
                  parsed.cameraName.c_str(), sender.name.c_str());
    sendTelegramMessageTo(sender.chatId, trUnknownCamera(sender.language, parsed.cameraName));
    return;
  }

  size_t i = matches[0];

  if (parsed.command == TelegramCommand::Snap) {
    Serial.printf("[%s] On-demand snapshot requested by user \"%s\" via Telegram.\n",
                  cameras[i].name.c_str(), sender.name.c_str());
    sendOnDemandSnapshot(cameras[i], states[i], sender.chatId, sender.language);
    return;
  }

  bool turnOn = (parsed.command == TelegramCommand::On);

  // parsed.durationText is "" for a plain /on or /off (permanent, the
  // original behavior) - only non-empty when a timer token followed the
  // camera name (see parseTelegramCommand's comment). See resolveAlertTimer's
  // own comment for why the actual parsing lives there, shared with the
  // "/on all"/"/off all" path (handleAllCamerasCommand) above.
  AlertTimer timer = resolveAlertTimer(parsed.durationText, turnOn, sender.language);
  if (!timer.ok) {
    Serial.printf("[Telegram] /%s target \"%s\" from user \"%s\" has an unparseable duration \"%s\".\n",
                  verb.c_str(), cameras[i].name.c_str(), sender.name.c_str(), parsed.durationText.c_str());
    sendTelegramMessageTo(sender.chatId, timer.errorMsg);
    return;
  }

  applyOnOffToCamera(cameras[i], states[i], i, turnOn, timer, sender.name, sender.chatId, sender.language);
}

// Handles an inline-keyboard button tap (sendCameraPickerKeyboard above) -
// upd.callbackData is "<verb>|<cameraNameOrAll>", e.g. "off|D01-FrontDoor".
// `sender` has already passed pollTelegramCommands' general permission
// gate, but this still re-checks the SPECIFIC permission the tapped verb
// needs (canCommand for on/off, canSnap for snap) - callback_data is
// client-supplied and never trusted alone.
static void handleTelegramCallbackQuery(const TelegramUser& sender, const TelegramUpdate& upd,
                                         const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  int sep = upd.callbackData.indexOf('|');
  String verb = sep >= 0 ? upd.callbackData.substring(0, sep) : upd.callbackData;
  String target = sep >= 0 ? upd.callbackData.substring(sep + 1) : "";

  // Handled separately from the on/off/snap camera-verb dispatch below -
  // this isn't a camera command at all (no cameras[]/states[] involved),
  // and needs no canCommand/canSnap check: a user's own display language
  // is available unconditionally, same as /lang's own text-command
  // counterpart (requiredPermissionForCommand(Lang) == Unknown).
  if (verb == "lang") {
    String targetLower = target;
    targetLower.toLowerCase();
    if (targetLower == "en") {
      applyLanguageChange(sender, TelegramLang::English);
    } else if (targetLower == "pt") {
      applyLanguageChange(sender, TelegramLang::Portuguese);
    } else {
      Serial.printf("[Telegram] Unrecognized /lang callback target \"%s\" from user \"%s\".\n",
                    target.c_str(), sender.name.c_str());
      answerTelegramCallback(upd.callbackQueryId, trCallbackUnrecognized(sender.language));
      return;
    }
    answerTelegramCallback(upd.callbackQueryId, "");
    return;
  }

  TelegramCommand command;
  if (verb == "on") command = TelegramCommand::On;
  else if (verb == "off") command = TelegramCommand::Off;
  else if (verb == "snap") command = TelegramCommand::Snap;
  else {
    Serial.printf("[Telegram] Unrecognized callback_data \"%s\" from user \"%s\".\n",
                  upd.callbackData.c_str(), sender.name.c_str());
    answerTelegramCallback(upd.callbackQueryId, trCallbackUnrecognized(sender.language));
    return;
  }

  bool authorized = (command == TelegramCommand::Snap) ? sender.canSnap : sender.canCommand;
  if (!authorized) {
    Serial.printf("[Telegram] User \"%s\" not authorized for the %s button.\n", sender.name.c_str(), verb.c_str());
    answerTelegramCallback(upd.callbackQueryId, trCallbackNotAuthorized(sender.language));
    sendTelegramMessageTo(sender.chatId, trNotAuthorized(sender.language, commandDisplayName(command)));
    return;
  }

  String targetLower = target;
  targetLower.toLowerCase();
  if (targetLower == "all") {
    // Reuses handleAllCamerasCommand as-is - it only reads parsed.command
    // (and parsed.durationText, always "" here - buttons are permanent
    // only), never parsed.cameraName, so a synthetic ParsedTelegramCommand
    // built just for this call is safe.
    ParsedTelegramCommand parsed;
    parsed.command = command;
    parsed.requiredPermission = requiredPermissionForCommand(command);
    handleAllCamerasCommand(sender, parsed, cameras, states, numCameras);
    answerTelegramCallback(upd.callbackQueryId, "");
    return;
  }

  // Exact match, not matchCamerasByPrefix - the button's label was this
  // camera's real name, generated by sendCameraPickerKeyboard itself, not
  // typed by hand, so there's no prefix-ambiguity case to handle here.
  // Still requires cameras[i].enabled, same as matchCamerasByPrefix does
  // for the text-command path - the camera list can change between the
  // picker being sent and a button being tapped, and a stale button for a
  // camera that's since been disabled must not silently still apply.
  int idx = -1;
  for (size_t i = 0; i < numCameras; i++) {
    if (cameras[i].enabled && cameras[i].name.equalsIgnoreCase(target)) { idx = (int)i; break; }
  }
  if (idx < 0) {
    // The camera list can change between the picker being sent and a
    // button being tapped (renamed/deleted/disabled, reboot required to
    // apply - see webserver_cameras.cpp) - handled as a clean "no longer
    // available" reply, not a crash.
    Serial.printf("[Telegram] Callback target camera \"%s\" no longer available (user \"%s\").\n",
                  target.c_str(), sender.name.c_str());
    answerTelegramCallback(upd.callbackQueryId, trCallbackCameraGone(sender.language));
    sendTelegramMessageTo(sender.chatId, trCameraNoLongerAvailable(sender.language, target));
    return;
  }

  if (command == TelegramCommand::Snap) {
    Serial.printf("[%s] On-demand snapshot requested by user \"%s\" via button.\n",
                  cameras[idx].name.c_str(), sender.name.c_str());
    sendOnDemandSnapshot(cameras[idx], states[idx], sender.chatId, sender.language);
    answerTelegramCallback(upd.callbackQueryId, "");
    return;
  }

  bool turnOn = (command == TelegramCommand::On);
  AlertTimer permanent; // default-constructed: ok=true, hasTimer=false, suffix="" - buttons are permanent only
  applyOnOffToCamera(cameras[idx], states[idx], (size_t)idx, turnOn, permanent, sender.name, sender.chatId,
                      sender.language);
  answerTelegramCallback(upd.callbackQueryId, "");
}

// Called once per loop() tick (main.cpp), unconditionally - unlike
// sendHeartbeat/checkNvsUsage/checkWifiSignal there, not gated behind an
// interval of its own. Cheap when nothing's due: just a millis()
// comparison per camera. Overflow-safe comparison (see CameraState::
// scheduledRevertDueMs's comment) matches main.cpp's own g_wifiRetryDueMs
// pattern. NOT cheap once something IS due, though - sendTelegramMessage
// below fans out to every systemMessages recipient (each up to a 45s
// g_telegramNetMutex wait - see that function's own comment) and is
// called separately for EVERY camera whose timer expires in the same
// tick, so several cameras timing out together (e.g. several muted with
// the same duration via the Cameras page's Mute all) is a real nested
// worst case for loop()'s 90s task watchdog. sendTelegramMessage now
// resets it per recipient internally, but this loop also resets after
// each camera's own revert completes, matching the same per-iteration
// feeding handleAllCamerasCommand's "/snap all" loop already does.
void checkScheduledAlertReverts(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  for (size_t i = 0; i < numCameras; i++) {
    unsigned long dueMs;
    bool revertToOn;
    {
      CameraStateLock lock(states[i]);
      dueMs = states[i].scheduledRevertDueMs;
      revertToOn = states[i].scheduledRevertToOn;
    }
    if (dueMs == 0) continue; // no timer pending for this camera
    if ((long)(millis() - dueMs) < 0) continue; // not due yet

    { CameraStateLock lock(states[i]); states[i].alertsEnabled = revertToOn; states[i].scheduledRevertDueMs = 0; }
    saveAlertEnabledPref(i, revertToOn);
    Serial.printf("[%s] Timed alert window expired - alerts turned %s automatically.\n",
                  cameras[i].name.c_str(), revertToOn ? "ON" : "OFF");
    logEvent(String(cameras[i].name) + " alerts: " + (revertToOn ? "ON" : "OFF") + " (timer expired)");
    String cameraName = cameras[i].name;
    sendTelegramMessage([cameraName, revertToOn](TelegramLang lang) {
      return trAlertsState(lang, cameraName, revertToOn, trTimerExpiredSuffix(lang));
    });
    esp_task_wdt_reset();
  }
}

// Bot API's own two-step file download (https://core.telegram.org/bots/api#getfile):
// getFile resolves fileId to a short-lived file_path, then a second GET
// to a different URL path (same host, so the same TELEGRAM_ROOT_CA
// applies) actually returns the bytes - two separate HTTPS round trips,
// not something this project chose, that's just how the Bot API works.
// Bounded by RESTORE_MAX_FILE_BYTES (config.h), checked against getFile's
// own reported file_size before ever starting the download, and again
// against the download response's own Content-Length before buffering
// it - a config export is at most a few tens of KB even with many
// cameras/users, so anything wildly larger is rejected outright rather
// than risking a large heap allocation from an oversized upload. Returns
// "" (Serial-only, no user-facing alert here - handleTelegramDocument
// composes that) on any failure: getFile itself failing, an oversized
// file, or the download failing.
static String downloadTelegramDocument(const String& fileId) {
  TelegramNetLock netLock;
  if (!netLock.held()) {
    Serial.println("[Telegram] downloadTelegramDocument: timed out waiting for Telegram send capacity.");
    return "";
  }

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA);
  client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000); // seconds, not ms - see sendTelegramPhotoBuffered's comment

  String filePath;
  {
    HTTPClient http;
    String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) + "/getFile?file_id=" + fileId;
    if (!http.begin(client, url)) {
      Serial.println("[Telegram] downloadTelegramDocument: getFile http.begin() failed.");
      return "";
    }
    http.setTimeout(HTTP_TIMEOUT_MS);
    int code = http.GET();
    if (code != 200) {
      Serial.printf("[Telegram] downloadTelegramDocument: getFile HTTP %d\n", code);
      http.end();
      return "";
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    // .c_str(), not body directly - same ArduinoJson/ArduinoFake reasoning
    // as parseTelegramUpdates (lib/telegram_parse).
    if (deserializeJson(doc, body.c_str()) != DeserializationError::Ok || !(doc["ok"] | false)) {
      Serial.println("[Telegram] downloadTelegramDocument: getFile response wasn't valid/ok.");
      return "";
    }
    long fileSize = doc["result"]["file_size"] | 0L;
    if (fileSize > (long)RESTORE_MAX_FILE_BYTES) {
      Serial.printf("[Telegram] downloadTelegramDocument: file too large (%ld bytes, max %u).\n", fileSize,
                    (unsigned)RESTORE_MAX_FILE_BYTES);
      return "";
    }
    const char* path = doc["result"]["file_path"];
    if (path == nullptr) {
      Serial.println("[Telegram] downloadTelegramDocument: getFile response had no file_path.");
      return "";
    }
    filePath = String(path);
  }

  HTTPClient http;
  String downloadUrl = "https://api.telegram.org/file/bot" + String(TELEGRAM_BOT_TOKEN) + "/" + filePath;
  if (!http.begin(client, downloadUrl)) {
    Serial.println("[Telegram] downloadTelegramDocument: download http.begin() failed.");
    return "";
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[Telegram] downloadTelegramDocument: download HTTP %d\n", code);
    http.end();
    return "";
  }
  int len = http.getSize();
  if (len > (int)RESTORE_MAX_FILE_BYTES) {
    Serial.printf("[Telegram] downloadTelegramDocument: download too large (%d bytes, max %u).\n", len,
                  (unsigned)RESTORE_MAX_FILE_BYTES);
    http.end();
    return "";
  }
  String content = http.getString();
  http.end();
  return content;
}

// The /restore flow's second step - a document (file) upload. Two ways to
// trigger an actual restore attempt, both requiring sender.canRestore
// (checked here explicitly, not just by the coarse "has at least one
// permission" gate pollTelegramCommands already applied before ever
// calling this):
//   - One-step: the document's own caption is exactly "/restore" - no
//     prior arming needed, for a sender who attaches the file and types
//     the command in the same message.
//   - Two-step: a bare "/restore" (TelegramCommand::Restore, above) armed
//     g_pendingRestoreChatId for this exact chat, and this document
//     arrived within RESTORE_PENDING_WINDOW_MS of that.
// Any OTHER document (no matching caption, no valid pending arm) is
// silently ignored - same "not something we act on" treatment
// pollTelegramCommands already gives a captionless sticker/photo -
// EXCEPT when this chat had armed a restore that has since expired, which
// gets an explicit trRestoreExpired reply instead of silence, since that
// is specifically the "I was following the flow but took too long" case
// worth explaining rather than leaving a confused sender guessing.
static void handleTelegramDocument(const TelegramUser& sender, const TelegramUpdate& update) {
  String caption = update.documentCaption;
  caption.trim();
  caption.toLowerCase();
  bool captionSaysRestore = caption == "/restore";

  bool pendingMatchesThisChat = g_pendingRestoreChatId.length() > 0 && g_pendingRestoreChatId == sender.chatId;
  bool pendingStillValid = pendingMatchesThisChat && (long)(millis() - g_pendingRestoreExpiresAtMs) < 0;

  if (!captionSaysRestore && !pendingStillValid) {
    if (pendingMatchesThisChat) { // was armed for this chat, but the window has since lapsed
      g_pendingRestoreChatId = "";
      sendTelegramMessageTo(sender.chatId, trRestoreExpired(sender.language));
    }
    return; // not a restore request at all - e.g. an unrelated file upload
  }

  // Consumed regardless of outcome below - a stale arm must never survive
  // to (mis)match a LATER, unrelated document from the same chat.
  if (pendingMatchesThisChat) g_pendingRestoreChatId = "";

  if (!sender.canRestore) {
    Serial.printf("[Telegram] User \"%s\" not authorized for /restore.\n", sender.name.c_str());
    sendTelegramMessageTo(sender.chatId, trNotAuthorized(sender.language, commandDisplayName(TelegramCommand::Restore)));
    return;
  }

  Serial.printf("[Telegram] Restore file received from user \"%s\" (%s) - downloading...\n", sender.name.c_str(),
                update.documentFileName.c_str());
  String content = downloadTelegramDocument(update.documentFileId);
  if (content.length() == 0) {
    Serial.printf("[Telegram] Restore download for user \"%s\" failed.\n", sender.name.c_str());
    sendTelegramMessageTo(sender.chatId, trBackupFailed(sender.language)); // same generic "try again" wording
    return;
  }

  ConfigImportApplyResult result = applyConfigImport(content);
  String summary = summarizeImportResult(result, ImportSummaryFormat::PlainText);
  Serial.printf("[Telegram] Restore applied for user \"%s\": %s\n", sender.name.c_str(), summary.c_str());
  logEvent("Config restored via Telegram by " + sender.name);
  sendTelegramMessageTo(sender.chatId, trRestoreResult(sender.language, summary));
}

void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  // -1 sentinel: load the real value from NVS on this function's first
  // call only, rather than starting at 0 every boot - see this section's
  // top comment for why a reboot redelivering old updates is dangerous
  // now that /reset exists. Real Telegram update_ids are always >= 0.
  static long lastUpdateId = -1;
  if (lastUpdateId < 0) lastUpdateId = loadLastUpdateId();
  long startingUpdateId = lastUpdateId; // so the end-of-poll persist below is skipped when nothing advanced

  // HTTPClient rather than a raw socket + hand-rolled "\r\n\r\n" body split
  // (see sendTelegramMessageTo's comment) - that split silently mis-parses
  // if Telegram ever sends a chunked response, since it doesn't decode
  // chunk-size markers before handing the body to parseTelegramUpdates.
  String body;
  {
    // Scoped tightly to the network fetch only, released BEFORE the
    // update-dispatch loop below - that loop calls handleTelegramCommand/
    // handleTelegramCallbackQuery, which can themselves call back into
    // sendTelegramMessageTo/sendOnDemandSnapshot and so re-acquire
    // g_telegramNetMutex. xSemaphoreCreateMutex() is non-recursive -
    // holding the lock across that loop would have this same task block
    // trying to re-take a mutex it already owns. See g_telegramNetMutex's
    // own comment for the mutex's overall purpose.
    TelegramNetLock netLock;
    if (!netLock.held()) {
      Serial.println("[Telegram] pollTelegramCommands: timed out waiting for Telegram send capacity - "
                      "skipping this poll.");
      return; // transient - next poll (TELEGRAM_COMMAND_POLL_MS) retries
    }

    WiFiClientSecure client;
    client.setCACert(TELEGRAM_ROOT_CA);
    client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000); // seconds, not ms - see sendTelegramPhotoBuffered's comment

    HTTPClient http;
    String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) +
                 "/getUpdates?offset=" + String(lastUpdateId + 1) + "&timeout=0";
    if (!http.begin(client, url)) {
      Serial.println("[Telegram] pollTelegramCommands: http.begin() failed.");
      return;
    }
    http.setTimeout(HTTP_TIMEOUT_MS);

    int code = http.GET();
    if (code != 200) {
      String detail = (code > 0) ? String("") : (" - " + HTTPClient::errorToString(code));
      Serial.printf("[Telegram] pollTelegramCommands: HTTP %d%s\n", code, detail.c_str());
      http.end();
      return; // transient - next poll retries
    }
    body = http.getString();
    http.end();
  } // netLock released here - everything below runs unlocked, on purpose

  String parseError;
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body, &parseError);
  if (parseError.length() > 0) {
    Serial.printf("[Telegram] pollTelegramCommands: %s\n", parseError.c_str());
  }

  std::vector<TelegramUser> users = loadTelegramUsers();
  for (auto& upd : updates) {
    // Unlike sendHeartbeat/checkNvsUsage/checkWifiSignal/
    // checkScheduledAlertReverts (main.cpp/telegram_commands.cpp - each fires at
    // most once per loop() tick), `updates` can genuinely hold more than
    // one entry - several commands sent in a burst, several users
    // messaging around the same time, or the bot catching up after being
    // briefly offline. handleTelegramCommand/handleTelegramCallbackQuery
    // below can each independently block on g_telegramNetMutex for up to
    // 45s sending a reply, so this is the one loop in the file where
    // several such waits stacking up in a single call is the ordinary
    // case, not a rare timing coincidence. Reset unconditionally at the
    // top of every iteration (not just after a branch that sends
    // something) so it can't be skipped by one of this loop's several
    // `continue`s.
    esp_task_wdt_reset();

    // Advanced in RAM for every update (so the same batch isn't refetched
    // next poll), but NOT persisted to NVS here - Telegram delivers every
    // inbound message regardless of sender, so persisting per-update would
    // let anyone who finds this bot force an NVS write with no permission
    // required. Persisted once at the end of this loop instead (see this
    // section's top comment).
    if (upd.updateId > lastUpdateId) lastUpdateId = upd.updateId;
    if (!upd.hasChatId) continue; // no message on this update (edited_message, channel_post, ...)

    const TelegramUser* sender = nullptr;
    for (auto& u : users) {
      if (chatIdMatches(u.chatId, upd.chatId)) { sender = &u; break; }
    }
    // canCommand, canSnap, canReset, canBackup, and canRestore are
    // independent permissions (see TelegramUser) - a sender needs at
    // least one of them to reach handleTelegramCommand/
    // handleTelegramDocument at all; which specific commands that
    // actually unlocks is decided there, per-command.
    if (!sender || !(sender->canCommand || sender->canSnap || sender->canReset || sender->canBackup ||
                      sender->canRestore)) {
      if (sender) {
        Serial.printf("[Telegram] Ignored command from %s (not authorized to send commands)\n",
                      sender->name.c_str());
      } else {
        Serial.printf("[Telegram] Ignored command from unknown chat ID %lld\n", (long long)upd.chatId);
        recordUnknownChat(upd.chatId);
      }
      continue;
    }

    if (upd.hasCallbackQuery) {
      handleTelegramCallbackQuery(*sender, upd, cameras, states, numCameras);
      continue;
    }

    // Checked before the text-length short-circuit below - a document
    // upload legitimately has an empty upd.text (its own accompanying
    // text arrives as documentCaption instead, see TelegramUpdate's own
    // comment), so this would otherwise never be reached.
    if (upd.hasDocument) {
      handleTelegramDocument(*sender, upd);
      continue;
    }

    if (upd.text.length() == 0) continue; // e.g. a sticker or photo with no caption - nothing to act on

    // lastUpdateId passed through so handleTelegramCommand's /reset branch
    // can persist it immediately, before doing anything irreversible - see
    // that function's own top comment for why this isn't decided here.
    handleTelegramCommand(*sender, upd.text, cameras, states, numCameras, lastUpdateId);
  }
  if (lastUpdateId != startingUpdateId) saveLastUpdateId(lastUpdateId);
}
