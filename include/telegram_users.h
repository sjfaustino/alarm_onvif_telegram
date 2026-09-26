#pragma once
#include <Arduino.h>
#include <vector>

// Language for this user's Telegram messages (the dashboard is always
// English). English is the default, including for old records.
enum class TelegramLang : uint8_t { English = 0, Portuguese = 1 };

// A Telegram recipient, persisted in NVS ("tgusers"): which cameras it hears
// from, whether it gets system messages, and which commands it may send.
struct TelegramUser {
  String name;   // display label, unique key (like CameraConfig::name)
  String chatId; // numeric Telegram chat id, as a string

  // All cameras, including future ones; cameraNames is then ignored.
  bool allCameras = false;
  std::vector<String> cameraNames;

  bool systemMessages = false; // heartbeat + boot-online messages
  bool canCommand = false;     // may send /on, /off, /status

  // /snap is separate trust from muting alerts (canCommand).
  bool canSnap = false;

  // The permissions below are disruptive or sensitive, so they are off by
  // default even for the seeded Admin. /reset reboots the board.
  bool canReset = false;

  // /backup: the export includes camera credentials (never WiFi's).
  bool canBackup = false;

  // /restore: can replace every camera, user and network setting at once.
  bool canRestore = false;

  // Minimum gap between commands (60000/N ms), not a rolling window; 0 =
  // unlimited.
  uint16_t maxCommandsPerMinute = 0;

  TelegramLang language = TelegramLang::English;
};

// True if this user should receive alerts for camera `cameraName`.
bool telegramUserWantsCamera(const TelegramUser& user, const String& cameraName);

// Loads from NVS. A fresh board seeds one "Admin" from secrets.h's
// TELEGRAM_CHAT_ID with everything except the permissions above.
std::vector<TelegramUser> loadTelegramUsers();

// Overwrites the entire persisted user list.
bool saveTelegramUsers(const std::vector<TelegramUser>& users);

// Load-mutate-save helpers, keyed by name.
bool addTelegramUser(const TelegramUser& user);
bool deleteTelegramUser(const String& name);

// Replaces originalName with user (handles renames). Fails if not found or the
// new name collides.
bool updateTelegramUser(const String& originalName, const TelegramUser& user);

// Replaces the whole list under the store mutex (config import).
bool replaceAllTelegramUsers(const std::vector<TelegramUser>& users);
