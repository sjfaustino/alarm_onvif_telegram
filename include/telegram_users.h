#pragma once
#include <Arduino.h>
#include <vector>

// A Telegram recipient - persisted in NVS (Preferences, namespace
// "tgusers"), managed at runtime via the web UI's "Telegram Users" section.
// Replaces the old single hardcoded TELEGRAM_CHAT_ID: any number of chat
// IDs can now receive alerts, each independently configured for which
// cameras they hear from, whether they get heartbeat/boot messages, and
// whether they're allowed to send /on, /off, /status commands.
struct TelegramUser {
  String name;   // display label, unique key (like CameraConfig::name)
  String chatId; // numeric Telegram chat id, as a string

  // If true, this user gets every camera's alerts, including cameras added
  // later - cameraNames is ignored. If false, only the cameras named in
  // cameraNames.
  bool allCameras = false;
  std::vector<String> cameraNames;

  bool systemMessages = false; // heartbeat + boot-online messages
  bool canCommand = false;     // may send /on, /off, /status
};

// True if this user should receive alerts for camera `cameraName`.
bool telegramUserWantsCamera(const TelegramUser& user, const String& cameraName);

// Loads the Telegram user list from NVS. On the very first boot after
// upgrading to this (nothing in NVS yet), seeds a single user named "Admin"
// from secrets.h's TELEGRAM_CHAT_ID, with allCameras/systemMessages/
// canCommand all true - so the board keeps working exactly as before
// (one recipient, gets everything, can command) until you add more users
// or adjust that one through the web UI.
std::vector<TelegramUser> loadTelegramUsers();

// Overwrites the entire persisted user list.
bool saveTelegramUsers(const std::vector<TelegramUser>& users);

// Convenience wrappers used by the web UI - each loads, mutates, and saves
// in one call. addTelegramUser fails (returns false) if a user with that
// name already exists. deleteTelegramUser fails if no user with that name
// exists.
bool addTelegramUser(const TelegramUser& user);
bool deleteTelegramUser(const String& name);

// Replaces the user currently named originalName with user - user.name
// doesn't have to match originalName, so this also handles a rename. Fails
// (returns false, nothing saved) if originalName isn't found, or if
// user.name collides with a *different* existing user's name.
bool updateTelegramUser(const String& originalName, const TelegramUser& user);
