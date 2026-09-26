#pragma once
#include <Arduino.h>
#include <vector>
#include <time.h> // struct tm - parseDurationToken's "now" parameter
#include "camera_store.h" // CameraConfig

// One getUpdates result. Unusable updates (edits, channel posts, no text) are
// still returned so the caller can advance its offset; Telegram redelivers
// anything below it.
struct TelegramUpdate {
  long updateId = 0;
  // int64_t: real chat IDs exceed 32 bits (one came back as 0 in the field).
  int64_t chatId = 0;
  bool hasChatId = false;
  String text;

  // Inline-keyboard tap; chatId is filled the same way, text stays empty.
  bool hasCallbackQuery = false;
  String callbackQueryId; // needed to answer it (clears the button's loading spinner)
  String callbackData;    // e.g. "off|D01-FrontDoor" - see telegram_commands.cpp's handleTelegramCallbackQuery

  // File upload (used by /restore). Typed text arrives as documentCaption.
  // documentFileName is informational only.
  bool hasDocument = false;
  String documentFileId;
  String documentFileName;
  String documentCaption;
};

// Parses a getUpdates JSON body. On invalid JSON or an API error, sets *error
// and returns an empty vector.
std::vector<TelegramUpdate> parseTelegramUpdates(const String& jsonBody, String* error = nullptr);

// strtoll comparison - String::toInt() is 32-bit too.
bool chatIdMatches(const String& storedChatId, int64_t updateChatId);

// Case-insensitive name-prefix match over enabled cameras; returns all
// matches.
std::vector<size_t> matchCamerasByPrefix(const CameraConfig cameras[], size_t numCameras, const String& needle);

enum class TelegramCommand { Unknown, Status, Uptime, Reset, On, Off, Snap, Help, Health, Log, Lang, Backup, Restore };

// Permission each command needs - the single source for the authorization
// check (scattered checks caused the /reset reboot loop). The switches have no
// default, so a new command without a case fails the build (-Werror=switch in
// this lib's library.json).
enum class TelegramCommandPermission { Unknown, Command, Snap, Reset, Backup, Restore };
TelegramCommandPermission requiredPermissionForCommand(TelegramCommand command);

// Unknown (and empty cameraName) for anything unrecognized.
struct ParsedTelegramCommand {
  TelegramCommand command = TelegramCommand::Unknown;
  TelegramCommandPermission requiredPermission = TelegramCommandPermission::Unknown;
  String cameraName;

  // /on and /off: optional timer token ("30", "23:00"); interpreted later by
  // parseDurationToken.
  String durationText;

  // /log: optional count.
  String logCountText;

  // /lang: optional language code, validated by the handler.
  String langArgText;
};

// The one place command syntax is parsed (parsing twice caused the /reset
// loop). Case-insensitive. Bare /on, /off, /snap give cameraName "" (the
// handler shows a picker); for /on and /off, the rest after the camera goes
// into durationText.
ParsedTelegramCommand parseTelegramCommand(const String& text);

// Interprets a timer token against nowLocal (passed in for testability):
//   - plain minutes, capped at MAX_DURATION_MINUTES; works without NTP
//   - "HH:MM": next occurrence of that local time; needs a synced clock
struct ParsedDuration {
  // False on a bad token or out-of-range time, or for HH:MM before the clock
  // is synced.
  bool ok = false;
  unsigned long secondsFromNow = 0; // valid only if ok
};

// 14 days. A real limit: the due check `(long)(millis() - dueMs) < 0` only
// works under 2^31 ms (~24.8 days).
static const long MAX_DURATION_MINUTES = 20160; // 14 days
ParsedDuration parseDurationToken(const String& token, const struct tm& nowLocal);

// "/word" for a command ("" for Unknown). No default case, as above.
String commandDisplayName(TelegramCommand command);
