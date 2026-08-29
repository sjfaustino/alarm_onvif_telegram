#pragma once
#include <Arduino.h>
#include <vector>
#include <time.h> // struct tm - parseDurationToken's "now" parameter
#include "camera_store.h" // CameraConfig

// One entry from a Telegram getUpdates response's "result" array.
// updateId is always present; hasChatId/text may be empty/default for an
// update this project doesn't act on (an edited_message/channel_post, or a
// message with no text). Every update is still returned even when unusable,
// so the caller can advance its "highest update_id seen" offset - Telegram
// redelivers anything below that offset forever otherwise.
struct TelegramUpdate {
  long updateId = 0;
  // int64_t, not long: `long` is 32-bit here (max ~2.1 billion), and real
  // Telegram chat IDs for ordinary accounts routinely exceed that - a
  // 32-bit chatId silently came back as 0 for one in the field, matching
  // no configured user.
  int64_t chatId = 0;
  bool hasChatId = false;
  String text;

  // Set for an inline-keyboard button tap (callback_query) rather than a
  // typed message - chatId/hasChatId are populated the same way either
  // way, so pollTelegramCommands' existing sender-lookup needs no changes.
  // text stays empty for a callback update.
  bool hasCallbackQuery = false;
  String callbackQueryId; // needed to answer it (clears the button's loading spinner)
  String callbackData;    // e.g. "off|D01-FrontDoor" - see telegram.cpp's handleTelegramCallbackQuery
};

// Parses a Telegram getUpdates response body (JSON only - the caller
// strips HTTP headers first) into one TelegramUpdate per "result" entry,
// via ArduinoJson. If `error` is non-null, it's set when the body isn't
// valid JSON or the API reported failure (e.g. an invalid bot token) -
// both return an empty vector.
//
// Handles a typed "message" and a "callback_query" (inline-keyboard tap -
// populates hasCallbackQuery/callbackQueryId/callbackData). Any other
// shape (edited_message, channel_post) leaves hasChatId false but still
// returns updateId so its offset advances.
std::vector<TelegramUpdate> parseTelegramUpdates(const String& jsonBody, String* error = nullptr);

// Compares a TelegramUser's stored chat ID (persisted as text) against a
// parsed update's chat ID. Uses strtoll, not String::toInt() (also
// 32-bit) - same overflow risk as chatId above.
bool chatIdMatches(const String& storedChatId, int64_t updateChatId);

// Case-insensitive prefix match of `needle` against every *enabled*
// camera's name (e.g. "d01" matches "D01-FrontDoor") - shared by /on, /off,
// /snap's target-camera lookup. Returns every match; the caller decides
// what to do with 0, 1, or several.
std::vector<size_t> matchCamerasByPrefix(const CameraConfig cameras[], size_t numCameras, const String& needle);

// The specific command a message's text was recognized as - Unknown means
// it isn't a recognized command at all.
enum class TelegramCommand { Unknown, Status, Uptime, Reset, On, Off, Snap, Help, Health, Log };

// Which TelegramUser permission a command requires. The single source of
// truth handleTelegramCommand's authorization check is built from, instead
// of each command carrying its own scattered "if (!sender.canX)" check -
// the exact bug class that caused the /reset reboot loop. Both this
// switch and requiredPermissionForCommand's own implementation have no
// default case - a new TelegramCommand added without a case here is a
// build failure (-Werror=switch, scoped to this module's own
// library.json, not project-wide - see platformio.ini's comment).
enum class TelegramCommandPermission { Unknown, Command, Snap, Reset };
TelegramCommandPermission requiredPermissionForCommand(TelegramCommand command);

// One recognized command, already fully parsed. command is Unknown (and
// cameraName empty) for anything not recognized.
struct ParsedTelegramCommand {
  TelegramCommand command = TelegramCommand::Unknown;
  TelegramCommandPermission requiredPermission = TelegramCommandPermission::Unknown;
  String cameraName;

  // /on and /off only: an optional trailing token, e.g. "/off D01 30" or
  // "/on D01 23:00" - "" means no timer (permanent on/off). Not yet
  // interpreted as a duration - see parseDurationToken, which needs the
  // current local time and so can't live in this time-independent parser.
  String durationText;

  // /log only: the optional trailing count, e.g. "/log 20" -> "20"; ""
  // for a bare "/log". Dedicated field rather than reusing durationText -
  // same one-field-per-command reasoning as above.
  String logCountText;
};

// The single place message text is matched against command syntax -
// handleTelegramCommand used to parse this twice (once for permission,
// once to dispatch), which is what let the /reset reboot loop happen.
// Case-insensitive.
//
// Bare "/on"/"/off"/"/snap" (no trailing target) parse with cameraName ==
// "" - handleTelegramCommand sends an inline-keyboard camera picker for
// this instead of the usual name/prefix match (which would otherwise
// wrongly match every camera against ""). /on and /off additionally
// accept a second, space-separated token as a timer - everything after
// the camera name's first token goes into durationText verbatim (see
// parseDurationToken). Not available via the button picker (tap-to-toggle
// only).
ParsedTelegramCommand parseTelegramCommand(const String& text);

// Interprets a /on or /off duration token relative to nowLocal (the
// caller's current local time, passed explicitly so this stays
// deterministic to test). Two forms:
//  - A plain non-negative integer: minutes from now, capped at
//    MAX_DURATION_MINUTES. Doesn't touch nowLocal, so works before NTP
//    has ever synced.
//  - "HH:MM" (24h): seconds until the next local occurrence of that time -
//    today if still ahead of nowLocal, tomorrow otherwise - requires
//    nowLocal to actually be synced (see `ok` below).
// Call only when durationText is non-empty - the caller treats "" as "no
// timer" before reaching this function.
struct ParsedDuration {
  // False if the token was neither form above, an HH:MM value was out of
  // range, or (HH:MM only) nowLocal isn't synced yet (tm_year <= 2016) -
  // resolving "at 23:00" against an unsynced clock would silently
  // schedule against the wrong wall-clock time.
  bool ok = false;
  unsigned long secondsFromNow = 0; // valid only if ok
};

// Upper bound for the plain-minutes duration form - 14 days. Real bound,
// not a sanity number: checkScheduledAlertReverts (telegram.cpp) decides
// "is this timer due yet" via the standard millis()-wraparound-safe
// `(long)(millis() - dueMs) < 0` idiom, only correct for a delay under
// 2^31ms (~24.86 days) - a duration parsed past that would read as
// already-due the instant it's scheduled, reverting the camera to the
// opposite of what was requested within one loop() tick.
static const long MAX_DURATION_MINUTES = 20160; // 14 days
ParsedDuration parseDurationToken(const String& token, const struct tm& nowLocal);

// The canonical "/word" text for a recognized command, e.g. for "You're
// not authorized to use ___." - "" for Unknown. Same no-default-switch
// reasoning as requiredPermissionForCommand.
String commandDisplayName(TelegramCommand command);
