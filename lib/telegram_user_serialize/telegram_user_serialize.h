#pragma once
#include <Arduino.h> // explicit, not just via telegram_users.h - see camera_serialize.h's comment
#include "telegram_users.h"

// TelegramUser <-> NVS record, tested natively. Versioned like
// camera_serialize.h: to add a setting, append a row to the table in the .cpp
// and bump the version to the row's.
static const uint16_t TELEGRAM_USER_SCHEMA_VERSION = 6;

String serializeUser(const TelegramUser& u);

// recordVersion 0 = pre-versioning format. Returns an empty-name user if
// malformed; loadTelegramUsers skips those.
TelegramUser deserializeUser(const String& record, uint16_t recordVersion);

// For the parse-failure log only.
size_t telegramUserRecordFieldCount(const String& record);

// telegramUserWantsCamera() (declared in telegram_users.h) is implemented here
// too.
