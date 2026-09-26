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
// Telegram commands (short getUpdates polls from loop())
//
// lastUpdateId is persisted so a reboot doesn't redeliver commands: a
// redelivered /reset once caused an endless reboot loop. It's saved once per
// poll (not per update, which would let any stranger force NVS writes), and
// /reset saves it itself just before restarting.
// ============================================================

static const char* TELEGRAM_STATE_NAMESPACE = "tgstate";
static const char* TELEGRAM_STATE_KEY_LAST_UPDATE_ID = "lastUpdateId";

static long loadLastUpdateId() {
  Preferences prefs;
  // Read-write (see loadDashboardAuth).
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
// Recent unknown chat IDs, so the Users page can offer them for copy-paste.
// RAM-only fixed table; not a security log.
// ============================================================

// `used` marks an occupied slot.
struct UnknownChatSlot {
  int64_t chatId = 0;
  unsigned long lastSeenMs = 0;
  bool used = false;
};
static UnknownChatSlot g_unknownChats[UNKNOWN_CHAT_TRACK_MAX];
static SemaphoreHandle_t g_unknownChatsMutex = xSemaphoreCreateMutex();

// Exact match, else evict the least recently seen.
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
// Per-user command rate limit: a minimum gap between commands (one timestamp
// per chat). Same fixed-table eviction as above.
// ============================================================

static const size_t COMMAND_RATE_TRACK_MAX = 16; // generous for any realistic configured-user count

struct CommandRateSlot {
  String chatId;
  unsigned long lastCommandMs = 0;
  bool used = false;
};
static CommandRateSlot g_commandRateTable[COMMAND_RATE_TRACK_MAX];
static SemaphoreHandle_t g_commandRateMutex = xSemaphoreCreateMutex();

// Callers skip this when the limit is 0 (unlimited). Records and allows the
// command if the gap has passed; a new chat is always allowed.
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
  // Read-write (see loadDashboardAuth).
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

// One snapshot to whoever asked, ignoring mute and cooldown.
static void sendOnDemandSnapshot(const CameraConfig& cfg, CameraState& st, const String& chatId, TelegramLang lang) {
  // snapshotUri is written by the camera task, so read under the lock.
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

// Shared by single-camera and "all" /on|/off so timer parsing can't drift
// between them (duplicated parsing caused the /reset loop).
struct AlertTimer {
  bool ok = true;               // false only if durationText was non-empty and unparseable
  bool hasTimer = false;        // true if durationText was non-empty and DID parse
  unsigned long revertDueMs = 0; // valid only if hasTimer
  String suffix;                 // " (auto ON in 1h30m)" etc, "" if !hasTimer - appended to the reply
  String errorMsg;               // set only if !ok - what to reply with
};

// "" = no timer. HH:MM needs the local time, read here.
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

// Persists a /lang change (typed or tapped) and confirms in the new language.
// sender belongs to this poll's user list, so a copy is saved by name.
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

// Sets, persists, logs and confirms one camera's on/off - shared by the typed
// command and the picker button so both persist the same way.
static void applyOnOffToCamera(const CameraConfig& cfg, CameraState& st, size_t index, bool turnOn,
                                const AlertTimer& timer, const String& viaWho, const String& replyChatId,
                                TelegramLang lang) {
  {
    CameraStateLock lock(st); // read cross-task by camera.cpp/webserver.cpp
    st.alertsEnabled = turnOn;
    // A new command replaces any pending timer.
    st.scheduledRevertDueMs = timer.hasTimer ? timer.revertDueMs : 0;
    st.scheduledRevertToOn = !turnOn;
  }
  saveAlertEnabledPref(index, turnOn);
  Serial.printf("[%s] Alerts turned %s via Telegram by user \"%s\"%s.\n", cfg.name.c_str(),
                turnOn ? "ON" : "OFF", viaWho.c_str(), timer.hasTimer ? " (timed)" : "");
  logEvent(cfg.name + " alerts: " + (turnOn ? "ON" : "OFF") + " via " + viaWho + timer.suffix);
  sendTelegramMessageTo(replyChatId, trAlertsState(lang, cfg.name, turnOn, timer.suffix));
}

// Shared by /on|/off all and the dashboard's Mute/Unmute all. Returns the
// result text for the caller to show.
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

// /on all, /off all [duration], /snap all over every enabled camera.
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
      // Many slow cameras could add up past the 90s watchdog within one tick.
      esp_task_wdt_reset();
    }
    return;
  }

  bool turnOn = (parsed.command == TelegramCommand::On);
  String result = setAllCamerasAlertState(cameras, states, numCameras, turnOn, parsed.durationText,
                                           "Telegram (" + sender.name + ")", sender.language);
  sendTelegramMessageTo(sender.chatId, result);
}

// Picker for a bare /on, /off or /snap: one button per enabled camera plus
// "All", with callback_data "<verb>|<name or all>". Permanent on/off only.
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
  // The "all" token is protocol and stays untranslated.
  buttons.push_back({trAllButtonLabel(sender.language), verb + "|all"});

  size_t skipped = 0;
  sendTelegramKeyboardTo(sender.chatId, trCameraPickerPrompt(sender.language, commandDisplayName(command)),
                          buttons, &skipped);
  if (skipped > 0) {
    // Tell the user if any camera didn't fit, rather than leaving a silent
    // gap.
    sendTelegramMessageTo(sender.chatId,
                           trCallbackDataTooLong(sender.language, skipped, commandDisplayName(command)));
  }
}

// Chat that armed a bare /restore; its next document within
// RESTORE_PENDING_WINDOW_MS is the restore file. One slot (a second /restore
// replaces it). loop() task only, so no lock.
static String g_pendingRestoreChatId; // "" = none armed
static unsigned long g_pendingRestoreExpiresAtMs = 0;

// lastUpdateId is already past this update, so /reset can persist it.
//
// text is parsed once, by parseTelegramCommand. The switch has no default
// (-Werror=switch), so a new command without a case won't build.
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
  // Every rejected command is logged.
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
      // isOffline is written by camera tasks; read the whole group under one
      // lock.
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
      // Persist before restarting (see the section comment above).
      saveLastUpdateId(lastUpdateId);
      // Reply first - ESP.restart() never returns.
      sendTelegramMessageTo(sender.chatId, trRebootingNow(sender.language));
      delay(500); // let the TLS send above finish flushing before the reboot tears down WiFi
      // Don't restart mid SD write: FAT isn't journaled.
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
        // Newest first. Entry text stays English; only the prefix is
        // translated.
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

  // Bare command: show the picker (an empty prefix would match every camera).
  if (parsed.cameraName.length() == 0) {
    sendCameraPickerKeyboard(sender, parsed.command, cameras, numCameras);
    return;
  }

  // "all" = every enabled camera.
  String cameraNameLower = parsed.cameraName;
  cameraNameLower.toLowerCase();
  if (cameraNameLower == "all") {
    handleAllCamerasCommand(sender, parsed, cameras, states, numCameras);
    return;
  }

  // Prefix match; ambiguous matches are listed, not applied.
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

  AlertTimer timer = resolveAlertTimer(parsed.durationText, turnOn, sender.language);
  if (!timer.ok) {
    Serial.printf("[Telegram] /%s target \"%s\" from user \"%s\" has an unparseable duration \"%s\".\n",
                  verb.c_str(), cameras[i].name.c_str(), sender.name.c_str(), parsed.durationText.c_str());
    sendTelegramMessageTo(sender.chatId, timer.errorMsg);
    return;
  }

  applyOnOffToCamera(cameras[i], states[i], i, turnOn, timer, sender.name, sender.chatId, sender.language);
}

// Button tap: callbackData is "<verb>|<name or all>". Re-checks the verb's own
// permission, since callback_data comes from the client.
static void handleTelegramCallbackQuery(const TelegramUser& sender, const TelegramUpdate& upd,
                                         const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  int sep = upd.callbackData.indexOf('|');
  String verb = sep >= 0 ? upd.callbackData.substring(0, sep) : upd.callbackData;
  String target = sep >= 0 ? upd.callbackData.substring(sep + 1) : "";

  // Not a camera command, and needs no permission (like /lang).
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
    // handleAllCamerasCommand only reads command and durationText, so a
    // synthetic parse is safe.
    ParsedTelegramCommand parsed;
    parsed.command = command;
    parsed.requiredPermission = requiredPermissionForCommand(command);
    handleAllCamerasCommand(sender, parsed, cameras, states, numCameras);
    answerTelegramCallback(upd.callbackQueryId, "");
    return;
  }

  // Exact name (the button carries the real name), and still enabled - the
  // list may have changed since the picker was sent.
  int idx = -1;
  for (size_t i = 0; i < numCameras; i++) {
    if (cameras[i].enabled && cameras[i].name.equalsIgnoreCase(target)) { idx = (int)i; break; }
  }
  if (idx < 0) {
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

// Every loop() tick. Cheap unless something is due; then each revert
// broadcasts (up to 45s per recipient), so reset the watchdog after each
// camera.
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

// Bot API two-step download: getFile gives a short-lived path, then a second
// GET fetches the bytes. Size is checked against RESTORE_MAX_FILE_BYTES before
// and during the download. Returns "" on failure (the caller replies).
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
    // .c_str(): see parseTelegramUpdates.
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

// Second step of /restore. Requires canRestore, and either a caption of
// exactly "/restore" (one step) or a live arm from this chat (two steps).
// Other documents are ignored, except that an expired arm gets an explanation.
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

  // Consumed either way, so a stale arm can't match a later document.
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
  // -1: load from NVS on first call (see the section comment above).
  static long lastUpdateId = -1;
  if (lastUpdateId < 0) lastUpdateId = loadLastUpdateId();
  long startingUpdateId = lastUpdateId; // so the end-of-poll persist below is skipped when nothing advanced

  // HTTPClient handles chunked responses.
  String body;
  {
    // Released before dispatch: handlers send replies, which take this
    // (non-recursive) mutex again.
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
    // A batch can hold several commands, each reply able to wait 45s for the
    // mutex, so reset the watchdog at the top of every iteration (before any
    // `continue`).
    esp_task_wdt_reset();

    // Advanced in RAM per update; persisted once after the loop.
    if (upd.updateId > lastUpdateId) lastUpdateId = upd.updateId;
    if (!upd.hasChatId) continue; // no message on this update (edited_message, channel_post, ...)

    const TelegramUser* sender = nullptr;
    for (auto& u : users) {
      if (chatIdMatches(u.chatId, upd.chatId)) { sender = &u; break; }
    }
    // Needs at least one permission; each command checks its own.
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

    // Before the empty-text check: a document's text arrives as its caption.
    if (upd.hasDocument) {
      handleTelegramDocument(*sender, upd);
      continue;
    }

    if (upd.text.length() == 0) continue; // e.g. a sticker or photo with no caption - nothing to act on

    handleTelegramCommand(*sender, upd.text, cameras, states, numCameras, lastUpdateId);
  }
  if (lastUpdateId != startingUpdateId) saveLastUpdateId(lastUpdateId);
}
