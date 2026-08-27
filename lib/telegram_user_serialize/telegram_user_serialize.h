#pragma once
#include <Arduino.h> // explicit, not just via telegram_users.h - see camera_serialize.h's comment
#include "telegram_users.h"

// Pure (de)serialization between TelegramUser and the pipe/list-delimited
// record format telegram_users.cpp persists to NVS (one record per user,
// joined by telegram_users.cpp's own RECORD_SEP). Split out so it can be
// unit-tested natively (test/test_telegram_user_serialize) without pulling
// in <Preferences.h>, which only exists on-device.
//
// Schema-versioned the same way as camera_serialize.h - see that header's
// CAMERA_SCHEMA_VERSION comment for the full rationale. Bump this (and add
// a new deserializeUser branch, never edit an existing one) any time
// serializeUser()'s field layout changes.
static const uint16_t TELEGRAM_USER_SCHEMA_VERSION = 1;

// Always writes TELEGRAM_USER_SCHEMA_VERSION's current field layout.
String serializeUser(const TelegramUser& u);

// `recordVersion` is the schema version `record` was actually saved
// under (0 means "written before this versioning scheme existed" - the
// original field-count-tolerant format). Returns a default-constructed
// TelegramUser (name.length()==0) if `record` is malformed *for that
// version* - telegram_users.cpp's loadTelegramUsers() skips (and logs)
// any entry that comes back with an empty name.
TelegramUser deserializeUser(const String& record, uint16_t recordVersion);

// telegramUserWantsCamera() is declared in telegram_users.h (its natural
// public-API home) even though it's implemented in this module's .cpp -
// not (de)serialization, just plain business logic that happened to be
// small and pure enough to move alongside it.
