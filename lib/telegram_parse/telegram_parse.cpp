#include "telegram_parse.h"
#include <ArduinoJson.h>
#include <cstdlib>
#include <cctype>

std::vector<TelegramUpdate> parseTelegramUpdates(const String& jsonBody, String* error) {
  std::vector<TelegramUpdate> updates;

  JsonDocument doc;
  // .c_str(), not jsonBody directly - ArduinoJson's Arduino-String reader
  // specialization relies on platform detection that doesn't kick in
  // under ArduinoFake (env:native), falling back to a generic Stream-style
  // reader String doesn't implement. Passing a plain const char* sidesteps
  // that entirely and works identically on both the real firmware and
  // native tests.
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
      // const char*, not .as<String>() - same reasoning as the .c_str()
      // above: ArduinoJson's String-aware conversion isn't available under
      // ArduinoFake, but every platform supports const char*, and String's
      // own constructor from one works everywhere regardless.
      const char* text = message["text"];
      if (text != nullptr) {
        u.text = String(text);
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

      // Same null-safety idiom as the "message" branch above - a stale/
      // deleted-message callback_query can omit this entirely, in which
      // case chatId/hasChatId just stay whatever "message" above already
      // left them (false, since a callback_query update has no top-level
      // "message" of its own).
      JsonObject cbMessage = callbackQuery["message"];
      if (!cbMessage.isNull()) {
        JsonObject chat = cbMessage["chat"];
        if (!chat.isNull() && !chat["id"].isNull()) {
          u.chatId = chat["id"].as<int64_t>();
          u.hasChatId = true;
        }
      }
    }
    // Neither "message" nor "callback_query" (e.g. edited_message,
    // channel_post) - u keeps hasChatId=false/text=""/hasCallbackQuery=
    // false, which the caller treats as "nothing to act on", but updateId
    // is still returned so its offset still advances.

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
    case TelegramCommand::Unknown:
    case TelegramCommand::Help: return TelegramCommandPermission::Unknown;
  }
  return TelegramCommandPermission::Unknown; // unreachable if every enumerator above is handled
}

// Splits "D01 30" into name="D01", duration="30" (both trimmed); "D01"
// alone leaves duration empty. Only the first two whitespace-separated
// tokens matter - anything after a second space is silently dropped
// (parseDurationToken/telegram.cpp reject a garbled duration token on
// their own, no need to duplicate that here).
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
  } else {
    return result; // Unknown, requiredPermission stays Unknown too
  }

  result.cameraName.trim();
  result.logCountText.trim();
  result.requiredPermission = requiredPermissionForCommand(result.command);
  return result;
}

ParsedDuration parseDurationToken(const String& token, const struct tm& nowLocal) {
  ParsedDuration result;
  if (token.length() == 0) return result;

  int colon = token.indexOf(':');
  if (colon < 0) {
    // Plain minutes - require every character to be a digit, so a typo
    // like "30m" or "abc" doesn't silently parse as 0 via String::toInt().
    for (size_t i = 0; i < token.length(); i++) {
      if (!isdigit((unsigned char)token[i])) return result;
    }
    long minutes = token.toInt();
    if (minutes <= 0) return result; // "0" isn't a valid timer
    result.ok = true;
    result.secondsFromNow = (unsigned long)minutes * 60UL;
    return result;
  }

  // HH:MM - exactly 2 digits each, colon in the middle, nothing else.
  if (token.length() != 5 || colon != 2) return result;
  for (int i = 0; i < 5; i++) {
    if (i == 2) continue; // the colon itself
    if (!isdigit((unsigned char)token[i])) return result;
  }
  int hour = token.substring(0, 2).toInt();
  int minute = token.substring(3, 5).toInt();
  if (hour > 23 || minute > 59) return result;

  // Resolving "at HH:MM" needs a real current time-of-day - same synced-
  // clock check onvif_soap.cpp's isoTimeNow() uses (tm_year is still at
  // the epoch default until NTP has actually set the clock at least once).
  if (nowLocal.tm_year <= (2016 - 1900)) return result;

  int nowSecOfDay = nowLocal.tm_hour * 3600 + nowLocal.tm_min * 60 + nowLocal.tm_sec;
  int targetSecOfDay = hour * 3600 + minute * 60;
  int deltaSec = targetSecOfDay - nowSecOfDay;
  // Already passed, or exactly now - roll to tomorrow rather than firing
  // (or scheduling a same-instant no-op revert) immediately.
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
    case TelegramCommand::Unknown: return "";
  }
  return ""; // unreachable if every enumerator above is handled
}
