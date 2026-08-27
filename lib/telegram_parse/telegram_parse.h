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

// Which TelegramUser permission a command's text requires - the single
// source of truth handleTelegramCommand's authorization check is built
// from, so a new command can't be added there without this table knowing
// what it needs (previously each command had its own separate, easy-to-
// forget "if (!sender.canX)" check scattered through the function).
// Unknown means the text isn't a recognized command at all. Case-
// insensitive; /on, /off, /snap specifically require a trailing space and
// target (matching how handleTelegramCommand actually parses them) - bare
// "/on" with nothing after it is Unknown, same as any other unrecognized
// text, not a permission question.
enum class TelegramCommandPermission { Unknown, Command, Snap, Reset };
TelegramCommandPermission requiredPermissionForCommand(const String& text);
