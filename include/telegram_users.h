#pragma once
#include <Arduino.h>
#include <vector>

// Which language this user's Telegram messages (alerts and command
// replies alike) are composed in - see lib/telegram_i18n. Does NOT affect
// the web dashboard, which stays English regardless. English is the
// default so every existing user (and any hand-edited/imported NVS
// record with no language recorded yet) keeps behaving exactly as before.
enum class TelegramLang : uint8_t { English = 0, Portuguese = 1 };

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
  // silence its alerts). See telegram_commands.cpp for how each command is gated.
  bool canSnap = false;

  // May send /reset (reboots the board immediately - drops every camera's
  // active subscription and stops monitoring until it comes back up).
  // Independent of canCommand/canSnap and, unlike them, NOT granted to the
  // auto-seeded Admin user by default (see loadTelegramUsers()) - this one
  // has to be turned on deliberately from the dashboard even for the first
  // user, since it's disruptive rather than just informational/control.
  bool canReset = false;

  // May send /backup (sends the current config export - see
  // webserver_security.h's buildConfigExport - as a Telegram document).
  // Independent of canCommand/canSnap/canReset and, like canReset, NOT
  // granted to the auto-seeded Admin user by default (see
  // loadTelegramUsers()) - the export's machine-readable section includes
  // every camera's own username/password (WiFi credentials are never
  // exported at all, unlike camera ones - see buildConfigExport's own
  // comment), arguably more sensitive than a reboot, so this has to be
  // turned on deliberately even for the first user.
  bool canBackup = false;

  // May send /restore (uploads a config-export file back to the bot and
  // applies it - webserver_security.h's applyConfigImport, the same
  // machinery the dashboard's own Security page Import uses). Independent
  // of every permission above and, like canReset/canBackup, NOT granted
  // to the auto-seeded Admin user by default - applying a config
  // overwrite is arguably the single most disruptive thing any Telegram
  // command can do (it can replace every camera, user, and network
  // setting in one shot), so this is opt-in even more deliberately than
  // canBackup's own "just reads credentials out" risk.
  bool canRestore = false;

  // Enforced as a minimum gap between this user's commands
  // (60000/maxCommandsPerMinute ms - see telegram_commands.cpp's rate-limit check in
  // handleTelegramCommand), not a true rolling-window count: O(1) in-RAM
  // state per user (just a last-command timestamp), no ring buffer. 0 =
  // unlimited (default) - most users never need this; it exists for a
  // shared/less-trusted chat that could otherwise hammer the board.
  uint16_t maxCommandsPerMinute = 0;

  // Language this user's alerts and command replies are composed in - see
  // TelegramLang's own comment above.
  TelegramLang language = TelegramLang::English;
};

// True if this user should receive alerts for camera `cameraName`.
bool telegramUserWantsCamera(const TelegramUser& user, const String& cameraName);

// Loads the Telegram user list from NVS. On the very first boot (nothing
// in NVS yet), seeds a single "Admin" user from secrets.h's
// TELEGRAM_CHAT_ID: all cameras, system messages, can command, can snap -
// but NOT canReset, which even this first user has to enable deliberately
// from the dashboard afterward (see TelegramUser::canReset's own comment).
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

// Wholesale replace of the entire persisted list (config import - see
// webserver_security.cpp's applyConfigImport) - unlike calling
// saveTelegramUsers() directly, this takes the same mutex
// addTelegramUser/updateTelegramUser/deleteTelegramUser do, so an import
// landing at the same moment as a concurrent dashboard edit can't lose
// either change to the other.
bool replaceAllTelegramUsers(const std::vector<TelegramUser>& users);
