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
  long chatId = 0;
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

// Case-insensitive prefix match of `needle` against every *enabled*
// camera's name (e.g. "d01" matches "D01-FrontDoor") - shared by /on, /off,
// and /snap's target-camera lookup in telegram.cpp's handleTelegramCommand.
// Returns every match; the caller decides what to do with 0, 1, or several
// (handleTelegramCommand replies with the ambiguity list on >1, "unknown"
// on 0).
std::vector<size_t> matchCamerasByPrefix(const CameraConfig cameras[], size_t numCameras, const String& needle);
