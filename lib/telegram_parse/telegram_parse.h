#pragma once
#include <Arduino.h>
#include <vector>
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
};

// Parses a Telegram getUpdates response body (JSON only - the caller
// strips the HTTP headers first) into one TelegramUpdate per "result"
// entry, using ArduinoJson rather than hand-rolled brace-counting and
// substring search over the raw response text. If `error` is non-null,
// it's set to a short description when the body isn't valid JSON at all,
// or the API itself reported failure (e.g. "ok":false from an invalid bot
// token) - both cases return an empty vector.
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
enum class TelegramCommand { Unknown, Status, Uptime, Reset, On, Off, Snap };

// Which TelegramUser permission a command requires - /status, /uptime,
// /on, /off need canCommand; /snap needs canSnap; /reset needs canReset.
// The single source of truth handleTelegramCommand's authorization check
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
};

// The single place message text is matched against command syntax.
// handleTelegramCommand (telegram.cpp) used to do this same matching
// twice - once here (deciding what permission was needed) and again,
// separately, to actually dispatch the command - two independent parses
// of the same text that had to agree with each other. That's exactly the
// kind of drift that caused the /reset reboot loop (a third such
// duplication, since fixed). Case-insensitive; /on, /off, /snap
// specifically require a trailing space and target to be recognized -
// bare "/on" with nothing after it is Unknown, not a malformed /on.
ParsedTelegramCommand parseTelegramCommand(const String& text);

// The canonical "/word" text for a recognized command, e.g. for
// "You're not authorized to use ___." replies - "" for Unknown. Also a
// switch with no default, for the same reason as requiredPermissionForCommand.
String commandDisplayName(TelegramCommand command);
