#pragma once
#include <Arduino.h>
#include <vector>

// A Telegram recipient - persisted in NVS (Preferences, namespace
// "tgusers"), managed via the web UI's "Telegram Users" section. Any
// number of chat IDs can receive alerts, each independently configured
// for which cameras it hears from, heartbeat/boot messages, and which
// commands it may send.
struct TelegramUser {
  String name;   // display label, unique key (like CameraConfig::name)
  String chatId; // numeric Telegram chat id, as a string

  // If true, gets every camera's alerts (including ones added later) and
  // cameraNames is ignored. If false, only the cameras in cameraNames.
  bool allCameras = false;
  std::vector<String> cameraNames;

  bool systemMessages = false; // heartbeat + boot-online messages
  bool canCommand = false;     // may send /on, /off, /status

  // Independent of canCommand - may send /snap on demand. Split out since
  // it's a different kind of trust (check in on a camera right now vs.
  // silence its alerts). See telegram.cpp for how each command is gated.
  bool canSnap = false;
};

// True if this user should receive alerts for camera `cameraName`.
bool telegramUserWantsCamera(const TelegramUser& user, const String& cameraName);

// Loads the Telegram user list from NVS. On the very first boot (nothing
// in NVS yet), seeds a single "Admin" user from secrets.h's
// TELEGRAM_CHAT_ID with every permission enabled.
std::vector<TelegramUser> loadTelegramUsers();

// Overwrites the entire persisted user list.
bool saveTelegramUsers(const std::vector<TelegramUser>& users);

// Convenience wrappers used by the web UI - load, mutate, save in one
// call. addTelegramUser fails if the name already exists; deleteTelegramUser
// fails if it doesn't.
bool addTelegramUser(const TelegramUser& user);
bool deleteTelegramUser(const String& name);

// Replaces the user named originalName with user (user.name need not
// match, so this also handles renames). Fails if originalName isn't
// found, or user.name collides with a different existing user.
bool updateTelegramUser(const String& originalName, const TelegramUser& user);
