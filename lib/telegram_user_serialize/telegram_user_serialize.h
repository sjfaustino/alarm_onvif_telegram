#pragma once
#include <Arduino.h> // explicit, not just via telegram_users.h - see camera_serialize.h's comment
#include "telegram_users.h"

// Pure (de)serialization between TelegramUser and the pipe/list-delimited
// record format telegram_users.cpp persists to NVS (one record per user,
// joined by telegram_users.cpp's own RECORD_SEP). Split out so it can be
// unit-tested natively (test/test_telegram_user_serialize) without pulling
// in <Preferences.h>, which only exists on-device.
String serializeUser(const TelegramUser& u);

// Returns a default-constructed TelegramUser (name.length()==0) if
// `record` is malformed - telegram_users.cpp's loadTelegramUsers() skips
// any entry that comes back with an empty name.
TelegramUser deserializeUser(const String& record);
