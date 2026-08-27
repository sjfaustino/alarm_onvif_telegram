#pragma once
#include <Arduino.h>
#include <vector>
#include "camera_store.h" // CameraConfig

// JSON string escaping/unescaping for Telegram's sendMessage body and
// getUpdates responses - hand-rolled substring work, not a full JSON
// parser (matches this project's ONVIF-side XML handling in
// lib/xml_helpers - see that module's comment for the tradeoffs that come
// with that choice). Split out from telegram.cpp so it can be
// unit-tested natively (test/test_telegram_parse).
String jsonEscape(const String& in);

// Inverse of jsonEscape() - only handles what Telegram could plausibly
// send back in a text field (it's never round-tripping arbitrary JSON,
// just one string value already extracted from a "text" field).
String jsonUnescape(const String& in);

// Case-insensitive prefix match of `needle` against every *enabled*
// camera's name (e.g. "d01" matches "D01-FrontDoor") - shared by /on, /off,
// and /snap's target-camera lookup in telegram.cpp's handleTelegramCommand.
// Returns every match; the caller decides what to do with 0, 1, or several
// (handleTelegramCommand replies with the ambiguity list on >1, "unknown"
// on 0).
std::vector<size_t> matchCamerasByPrefix(const CameraConfig cameras[], size_t numCameras, const String& needle);
