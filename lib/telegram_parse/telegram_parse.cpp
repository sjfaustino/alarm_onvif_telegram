#include "telegram_parse.h"
#include <ArduinoJson.h>
#include <cstdlib>
#include <cctype>

std::vector<TelegramUpdate> parseTelegramUpdates(const String& jsonBody, String* error) {
  std::vector<TelegramUpdate> updates;

  JsonDocument doc;
  // .c_str(): ArduinoJson's String reader doesn't work under ArduinoFake
  // (native tests); const char* works everywhere.
  DeserializationError err = deserializeJson(doc, jsonBody.c_str());
  if (err) {
    if (error) *error = String("JSON parse failed: ") + err.c_str();
    return updates;
  }

  bool ok = doc["ok"] | false;
  if (!ok) {
    if (error) *error = String("Telegram API error: ") + (doc["description"] | "(no description)");
    return updates;
  }

  for (JsonObject update : doc["result"].as<JsonArray>()) {
    TelegramUpdate u;
    u.updateId = update["update_id"] | 0L;

    JsonObject message = update["message"];
    if (!message.isNull()) {
      JsonObject chat = message["chat"];
      if (!chat.isNull() && !chat["id"].isNull()) {
        u.chatId = chat["id"].as<int64_t>();
        u.hasChatId = true;
      }
      // const char* for the same reason.
      const char* text = message["text"];
      if (text != nullptr) {
        u.text = String(text);
      }

      JsonObject document = message["document"];
      if (!document.isNull()) {
        const char* fileId = document["file_id"];
        if (fileId != nullptr) {
          u.documentFileId = String(fileId);
          u.hasDocument = true;
        }
        const char* fileName = document["file_name"];
        if (fileName != nullptr) u.documentFileName = String(fileName);
        // A document's text arrives as "caption".
        const char* caption = message["caption"];
        if (caption != nullptr) u.documentCaption = String(caption);
      }
    }

    JsonObject callbackQuery = update["callback_query"];
    if (!callbackQuery.isNull()) {
      const char* id = callbackQuery["id"];
      if (id != nullptr) {
        u.callbackQueryId = String(id);
        u.hasCallbackQuery = true;
      }
      const char* data = callbackQuery["data"];
      if (data != nullptr) u.callbackData = String(data);

      // May be absent on a stale callback; chatId then stays unset.
      JsonObject cbMessage = callbackQuery["message"];
      if (!cbMessage.isNull()) {
        JsonObject chat = cbMessage["chat"];
        if (!chat.isNull() && !chat["id"].isNull()) {
          u.chatId = chat["id"].as<int64_t>();
          u.hasChatId = true;
        }
      }
    }
    // Other update types: nothing to act on, but updateId still advances the
    // offset.

    updates.push_back(u);
  }
  return updates;
}

bool chatIdMatches(const String& storedChatId, int64_t updateChatId) {
  return strtoll(storedChatId.c_str(), nullptr, 10) == updateChatId;
}

std::vector<size_t> matchCamerasByPrefix(const CameraConfig cameras[], size_t numCameras, const String& needle) {
  String lowerNeedle = needle;
  lowerNeedle.toLowerCase();

  std::vector<size_t> matches;
  for (size_t i = 0; i < numCameras; i++) {
    if (!cameras[i].enabled) continue;
    String haystack = cameras[i].name;
    haystack.toLowerCase();
    if (haystack.startsWith(lowerNeedle)) matches.push_back(i);
  }
  return matches;
}

TelegramCommandPermission requiredPermissionForCommand(TelegramCommand command) {
  switch (command) {
    case TelegramCommand::Status:
    case TelegramCommand::Uptime:
    case TelegramCommand::Health:
    case TelegramCommand::Log:
    case TelegramCommand::On:
    case TelegramCommand::Off:  return TelegramCommandPermission::Command;
    case TelegramCommand::Snap: return TelegramCommandPermission::Snap;
    case TelegramCommand::Reset: return TelegramCommandPermission::Reset;
    case TelegramCommand::Backup: return TelegramCommandPermission::Backup;
    case TelegramCommand::Restore: return TelegramCommandPermission::Restore;
    case TelegramCommand::Unknown:
    case TelegramCommand::Help:
    // Language is a personal preference, open to every user (like /help).
    case TelegramCommand::Lang: return TelegramCommandPermission::Unknown;
  }
  return TelegramCommandPermission::Unknown; // unreachable if every enumerator above is handled
}

// "D01 30" -> name "D01", duration "30". Anything after a second space is
// dropped; bad durations are rejected downstream.
static void splitNameAndDuration(const String& rest, String& name, String& duration) {
  String trimmed = rest;
  trimmed.trim();
  int sp = trimmed.indexOf(' ');
  if (sp < 0) {
    name = trimmed;
    duration = "";
    return;
  }
  name = trimmed.substring(0, sp);
  duration = trimmed.substring(sp + 1);
  duration.trim();
}

ParsedTelegramCommand parseTelegramCommand(const String& text) {
  ParsedTelegramCommand result;
  String lower = text;
  lower.toLowerCase();

  if (lower == "/status") {
    result.command = TelegramCommand::Status;
  } else if (lower == "/uptime") {
    result.command = TelegramCommand::Uptime;
  } else if (lower == "/reset") {
    result.command = TelegramCommand::Reset;
  } else if (lower == "/backup") {
    result.command = TelegramCommand::Backup;
  } else if (lower == "/restore") {
    result.command = TelegramCommand::Restore;
  } else if (lower == "/help") {
    result.command = TelegramCommand::Help;
  } else if (lower == "/health") {
    result.command = TelegramCommand::Health;
  } else if (lower == "/log") {
    result.command = TelegramCommand::Log;
  } else if (lower.startsWith("/log ")) {
    result.command = TelegramCommand::Log;
    result.logCountText = text.substring(5);
  } else if (lower == "/on") {
    result.command = TelegramCommand::On; // cameraName stays "" - picker, see this function's own comment
  } else if (lower.startsWith("/on ")) {
    result.command = TelegramCommand::On;
    splitNameAndDuration(text.substring(4), result.cameraName, result.durationText);
  } else if (lower == "/off") {
    result.command = TelegramCommand::Off;
  } else if (lower.startsWith("/off ")) {
    result.command = TelegramCommand::Off;
    splitNameAndDuration(text.substring(5), result.cameraName, result.durationText);
  } else if (lower == "/snap") {
    result.command = TelegramCommand::Snap;
  } else if (lower.startsWith("/snap ")) {
    result.command = TelegramCommand::Snap;
    result.cameraName = text.substring(6);
  } else if (lower == "/lang") {
    result.command = TelegramCommand::Lang; // langArgText stays "" - picker, see handleTelegramCommand
  } else if (lower.startsWith("/lang ")) {
    result.command = TelegramCommand::Lang;
    result.langArgText = text.substring(6);
  } else {
    return result; // Unknown, requiredPermission stays Unknown too
  }

  result.cameraName.trim();
  result.logCountText.trim();
  result.langArgText.trim();
  result.requiredPermission = requiredPermissionForCommand(result.command);
  return result;
}

ParsedDuration parseDurationToken(const String& token, const struct tm& nowLocal) {
  ParsedDuration result;
  if (token.length() == 0) return result;

  int colon = token.indexOf(':');
  if (colon < 0) {
    // All digits, so "30m" doesn't parse as 0.
    for (size_t i = 0; i < token.length(); i++) {
      if (!isdigit((unsigned char)token[i])) return result;
    }
    long minutes = token.toInt();
    if (minutes <= 0) return result; // "0" isn't a valid timer
    // Rejected, not clamped (see MAX_DURATION_MINUTES).
    if (minutes > MAX_DURATION_MINUTES) return result;
    result.ok = true;
    result.secondsFromNow = (unsigned long)minutes * 60UL;
    return result;
  }

  if (token.length() != 5 || colon != 2) return result;
  for (int i = 0; i < 5; i++) {
    if (i == 2) continue; // the colon itself
    if (!isdigit((unsigned char)token[i])) return result;
  }
  int hour = token.substring(0, 2).toInt();
  int minute = token.substring(3, 5).toInt();
  if (hour > 23 || minute > 59) return result;

  // HH:MM needs a synced clock (tm_year stays at the epoch until NTP runs).
  if (nowLocal.tm_year <= (2016 - 1900)) return result;

  int nowSecOfDay = nowLocal.tm_hour * 3600 + nowLocal.tm_min * 60 + nowLocal.tm_sec;
  int targetSecOfDay = hour * 3600 + minute * 60;
  int deltaSec = targetSecOfDay - nowSecOfDay;
  // Already passed or now: tomorrow.
  if (deltaSec <= 0) deltaSec += 24 * 3600;

  result.ok = true;
  result.secondsFromNow = (unsigned long)deltaSec;
  return result;
}

String commandDisplayName(TelegramCommand command) {
  switch (command) {
    case TelegramCommand::Status: return "/status";
    case TelegramCommand::Uptime: return "/uptime";
    case TelegramCommand::Reset:  return "/reset";
    case TelegramCommand::On:     return "/on";
    case TelegramCommand::Off:    return "/off";
    case TelegramCommand::Snap:   return "/snap";
    case TelegramCommand::Help:   return "/help";
    case TelegramCommand::Health: return "/health";
    case TelegramCommand::Log:    return "/log";
    case TelegramCommand::Lang:   return "/lang";
    case TelegramCommand::Backup: return "/backup";
    case TelegramCommand::Restore: return "/restore";
    case TelegramCommand::Unknown: return "";
  }
  return ""; // unreachable if every enumerator above is handled
}
