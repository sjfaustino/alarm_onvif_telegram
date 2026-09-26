#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes
#include <vector>
#include "webserver_html.h" // FormParams
#include "camera_store.h"
#include "telegram_users.h"

// The Telegram user Add/Edit form, pure so it can be tested natively.

// cams supplies the per-camera checkboxes.
String renderTelegramUserForm(const TelegramUser& v, const std::vector<CameraConfig>& cams, bool isEdit);
// Only cameras in cams can be selected.
TelegramUser parseUserForm(const FormParams& params, const std::vector<CameraConfig>& cams);
