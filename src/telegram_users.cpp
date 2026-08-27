#include "telegram_users.h"
#include "telegram_user_serialize.h"
#include "secrets.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "tgusers";
static const char* NVS_KEY_LIST  = "list";

// Separates whole user records within the NVS blob (distinct from
// telegram_user_serialize.cpp's own FIELD_SEP/LIST_SEP, private to that
// file - this file only ever joins/splits on RECORD_SEP).
static const char RECORD_SEP = '\x1E';

std::vector<TelegramUser> loadTelegramUsers() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  bool alreadyInitialized = prefs.isKey(NVS_KEY_LIST);
  String blob = alreadyInitialized ? prefs.getString(NVS_KEY_LIST, "") : "";
  prefs.end();

  if (!alreadyInitialized) {
    std::vector<TelegramUser> users;
    TelegramUser admin;
    admin.name = "Admin";
    admin.chatId = TELEGRAM_CHAT_ID;
    admin.allCameras = true;
    admin.systemMessages = true;
    admin.canCommand = true;
    admin.canSnap = true;
    users.push_back(admin);
    saveTelegramUsers(users);
    Serial.println("[telegram_users] First boot with NVS-backed user storage - seeded one "
                    "\"Admin\" user from secrets.h's TELEGRAM_CHAT_ID (all cameras, system "
                    "messages, can command, can snap).");
    return users;
  }

  std::vector<TelegramUser> users;
  int recStart = 0;
  for (int i = 0; i <= (int)blob.length(); i++) {
    if (i == (int)blob.length() || blob[i] == RECORD_SEP) {
      if (i > recStart) {
        TelegramUser u = deserializeUser(blob.substring(recStart, i));
        if (u.name.length() > 0) users.push_back(u);
      }
      recStart = i + 1;
    }
  }
  return users;
}

bool saveTelegramUsers(const std::vector<TelegramUser>& users) {
  String blob;
  for (size_t i = 0; i < users.size(); i++) {
    if (i > 0) blob += RECORD_SEP;
    blob += serializeUser(users[i]);
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  prefs.putString(NVS_KEY_LIST, blob);
  prefs.end();
  return true;
}

bool addTelegramUser(const TelegramUser& user) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  for (auto& u : users) {
    if (u.name.equalsIgnoreCase(user.name)) return false;
  }
  users.push_back(user);
  return saveTelegramUsers(users);
}

bool deleteTelegramUser(const String& name) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  for (size_t i = 0; i < users.size(); i++) {
    if (users[i].name.equalsIgnoreCase(name)) {
      users.erase(users.begin() + i);
      return saveTelegramUsers(users);
    }
  }
  return false;
}

bool updateTelegramUser(const String& originalName, const TelegramUser& user) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  int idx = -1;
  for (size_t i = 0; i < users.size(); i++) {
    if (users[i].name.equalsIgnoreCase(originalName)) { idx = (int)i; break; }
  }
  if (idx < 0) return false;

  for (size_t i = 0; i < users.size(); i++) {
    if ((int)i != idx && users[i].name.equalsIgnoreCase(user.name)) return false;
  }

  users[idx] = user;
  return saveTelegramUsers(users);
}
