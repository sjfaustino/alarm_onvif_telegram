#pragma once
#include <Arduino.h>
#include <vector>
#include <time.h> // struct tm - parseDurationToken's "now" parameter
#include "camera_store.h" // CameraConfig

// One entry from a Telegram getUpdates response's "result" array.
// updateId is always present; hasChatId/text may be empty/default for an
// update this project doesn't act on (e.g. a non-"message" update like an
// edited_message or channel_post, or a message with no text - a sticker,
// a photo with no caption). Every update is still returned even when
// unusable, so the caller can advance its own "highest update_id seen"
// offset for it - Telegram redelivers anything below that offset forever
// otherwise.
struct TelegramUpdate {
  long updateId = 0;
  // int64_t, not long: `long` is only 32-bit on this platform (max ~2.1
  // billion), and real Telegram chat IDs for ordinary accounts routinely
  // exceed that (not just group/channel IDs) - a 32-bit chatId silently
  // came back as 0 for one in the field, matching no configured user.
  int64_t chatId = 0;
  bool hasChatId = false;
  String text;

  // Set when this update is an inline-keyboard button tap
  // (update.callback_query) rather than a typed message - chatId/hasChatId
  // above are populated the same way either way (from callback_query
  // .message.chat.id), so the existing sender-lookup/authorization gate in
  // pollTelegramCommands (telegram.cpp) needs no changes for this. text
  // stays empty for a callback update.
  bool hasCallbackQuery = false;
  String callbackQueryId; // needed to answer it (clears the button's loading spinner)
  String callbackData;    // e.g. "off|D01-FrontDoor" - see telegram.cpp's handleTelegramCallbackQuery
};

// Parses a Telegram getUpdates response body (JSON only - the caller
// strips the HTTP headers first) into one TelegramUpdate per "result"
// entry, using ArduinoJson rather than hand-rolled brace-counting and
// substring search over the raw response text. If `error` is non-null,
// it's set to a short description when the body isn't valid JSON at all,
// or the API itself reported failure (e.g. "ok":false from an invalid bot
// token) - both cases return an empty vector.
//
// Handles two update shapes: a typed "message" (populates text/chatId as
// before) and a "callback_query" (an inline-keyboard button tap -
// populates hasCallbackQuery/callbackQueryId/callbackData, and chatId from
// callback_query.message.chat.id the same way). Neither, or a shape this
// project doesn't act on (edited_message, channel_post) - hasChatId stays
// false, updateId is still returned so its offset still advances.
std::vector<TelegramUpdate> parseTelegramUpdates(const String& jsonBody, String* error = nullptr);

// Compares a TelegramUser's stored chat ID (persisted as text, see
// telegram_users.h) against a parsed update's chat ID. Uses strtoll
// rather than Arduino's String::toInt() (also only 32-bit on this
// platform) - same overflow risk as chatId above, so both sides of this
// comparison need to be 64-bit or a large chat ID silently matches nothing.
bool chatIdMatches(const String& storedChatId, int64_t updateChatId);

// Case-insensitive prefix match of `needle` against every *enabled*
// camera's name (e.g. "d01" matches "D01-FrontDoor") - shared by /on, /off,
// and /snap's target-camera lookup in telegram.cpp's handleTelegramCommand.
// Returns every match; the caller decides what to do with 0, 1, or several
// (handleTelegramCommand replies with the ambiguity list on >1, "unknown"
// on 0).
std::vector<size_t> matchCamerasByPrefix(const CameraConfig cameras[], size_t numCameras, const String& needle);

// The specific command a message's text was recognized as - Unknown means
// it isn't a recognized command at all.
enum class TelegramCommand { Unknown, Status, Uptime, Reset, On, Off, Snap, Help, Health, Log };

// Which TelegramUser permission a command requires - /status, /uptime,
// /health, /log, /on, /off need canCommand; /snap needs canSnap; /reset needs
// canReset; /help needs none (Unknown, same value the fallback "not a
// recognized command at all" case uses - a legitimate double meaning:
// Unknown means "no specific permission maps to this command," true for
// both cases, not "command is unknown" specifically). The single source
// of truth handleTelegramCommand's authorization check
// is built from, so a new TelegramCommand can't be wired up without this
// table knowing what it needs (previously each command had its own
// separate, easy-to-forget "if (!sender.canX)" check scattered through
// the function - the exact class of bug that caused the /reset reboot
// loop). Deliberately a switch with no default in both its declaration
// site's usage and requiredPermissionForCommand's own implementation - a
// new TelegramCommand value added without a case here is a build failure,
// not a silent fall-through to Unknown/unauthorized. That's only actually
// true because this module's own library.json sets -Werror=switch for it
// specifically (this platform's default flags don't enable -Wswitch at
// all otherwise, verified - see platformio.ini's build_src_flags comment
// for why it's set per-module instead of project-wide: the project-wide
// version of this fix applied the flag to every third-party library and
// the Arduino/ESP-IDF framework too, which could hard-fail the build over
// a non-exhaustive switch in code nobody here controls).
enum class TelegramCommandPermission { Unknown, Command, Snap, Reset };
TelegramCommandPermission requiredPermissionForCommand(TelegramCommand command);

// One recognized command, already fully parsed: which command it is, the
// permission it requires (via requiredPermissionForCommand), and - for
// /on, /off, /snap - the camera name/prefix that followed it, trimmed.
// command is Unknown (and cameraName empty) for anything not recognized.
struct ParsedTelegramCommand {
  TelegramCommand command = TelegramCommand::Unknown;
  TelegramCommandPermission requiredPermission = TelegramCommandPermission::Unknown;
  String cameraName;

  // /on and /off only: an optional trailing token after the camera name,
  // e.g. "/off D01 30" or "/on D01 23:00" - "" means no timer was given
  // (permanent on/off, the original behavior). Not yet interpreted as a
  // duration - see parseDurationToken below, which needs the current local
  // time and so can't live in this otherwise time-independent parser.
  // Unused (always "") for every other command.
  String durationText;

  // /log only: the optional trailing count, e.g. "/log 20" -> "20"; ""
  // for a bare "/log" (telegram.cpp defaults and clamps it). Unused
  // (always "") for every other command - dedicated field rather than
  // reusing durationText, same "one field per command's own specific
  // need" reasoning that field's own comment documents.
  String logCountText;
};

// The single place message text is matched against command syntax.
// handleTelegramCommand (telegram.cpp) used to do this same matching
// twice - once here (deciding what permission was needed) and again,
// separately, to actually dispatch the command - two independent parses
// of the same text that had to agree with each other. That's exactly the
// kind of drift that caused the /reset reboot loop (a third such
// duplication, since fixed). Case-insensitive.
//
// Bare "/on"/"/off"/"/snap" (no trailing target at all) parse as their
// respective command WITH cameraName == "" - handleTelegramCommand
// recognizes this and sends an inline-keyboard camera picker instead of
// falling through to the usual name/prefix matching (which would
// otherwise wrongly treat "" as matching every camera). "/on " (trailing
// space, still nothing after it) produces the same empty cameraName via
// the normal splitNameAndDuration path below - both are the same "no
// target given" case, just reached two different ways.
//
// /on and /off additionally accept a second, space-separated token as a
// timer, e.g. "/off D01 30" - everything after the camera name's own
// first token goes into durationText verbatim (see parseDurationToken for
// what it accepts), not re-split or validated here. Not available via the
// button picker - buttons are tap-to-toggle only, permanent on/off.
ParsedTelegramCommand parseTelegramCommand(const String& text);

// Interprets a /on or /off duration token (ParsedTelegramCommand::
// durationText) relative to nowLocal, the caller's current local time
// (telegram.cpp passes real localtime_r() output - kept as an explicit
// parameter, not read internally, so this stays deterministic to test).
// Two accepted forms:
//  - A plain non-negative integer: minutes from now (e.g. "30" -> 1800s),
//    capped at MAX_DURATION_MINUTES (see its own comment for why). Doesn't
//    touch nowLocal at all, so this form works even before NTP has ever
//    synced.
//  - "HH:MM" (24h, always 2+2 digits, e.g. "23:00" or "07:05"): seconds
//    until the next local wall-clock occurrence of that time - today if
//    it's still later than nowLocal, tomorrow if that time has already
//    passed (or is exactly now) - requires nowLocal to actually be
//    synced (see the note on `ok` below).
// Call only when durationText is non-empty - the caller (telegram.cpp)
// treats "" as "no timer" before ever reaching this function, which has
// no representation for "no duration" of its own.
struct ParsedDuration {
  // False if token was neither form above, an HH:MM value was out of
  // range, or (HH:MM only) nowLocal doesn't look synced yet (tm_year <=
  // 2016, this platform's/telegram.cpp's own "hasn't heard from NTP"
  // check) - resolving "at 23:00" against an unsynced clock would silently
  // schedule against the wrong wall-clock time.
  bool ok = false;
  unsigned long secondsFromNow = 0; // valid only if ok
};

// Upper bound for the plain-minutes form of a duration token - 14 days,
// comfortably generous for an on/off mute timer. Real bound, not just a
// sanity number: telegram.cpp's checkScheduledAlertReverts decides "is
// this timer due yet" via `(long)(millis() - dueMs) < 0`, the standard
// millis()-wraparound-safe idiom - which is only correct for a scheduled
// delay under 2^31ms (~24.86 days). A duration parsed past that point
// would make the due-check read as "already due" the moment it's
// scheduled, silently reverting the camera to the OPPOSITE of what was
// requested within one loop() tick instead of after the requested delay -
// a real bug this cap exists to make unreachable, not a defensive
// nicety.
static const long MAX_DURATION_MINUTES = 20160; // 14 days
ParsedDuration parseDurationToken(const String& token, const struct tm& nowLocal);

// The canonical "/word" text for a recognized command, e.g. for
// "You're not authorized to use ___." replies - "" for Unknown. Also a
// switch with no default, for the same reason as requiredPermissionForCommand.
String commandDisplayName(TelegramCommand command);
