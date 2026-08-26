#include "telegram_users.h"
#include "secrets.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "tgusers";
static const char* NVS_KEY_LIST  = "list";

// Same non-printable ASCII separator scheme as camera_store.cpp: no user
// name, chat id, or camera name should ever legitimately contain these.
static const char FIELD_SEP  = '\x1F'; // between a user's fields
static const char RECORD_SEP = '\x1E'; // between users
static const char LIST_SEP   = '\x1D'; // between camera names within cameraNames

static String stripSeparators(const String& s) {
  String out = s;
  out.replace(String(FIELD_SEP), "");
  out.replace(String(RECORD_SEP), "");
  out.replace(String(LIST_SEP), "");
  return out;
}

bool telegramUserWantsCamera(const TelegramUser& user, const String& cameraName) {
  if (user.allCameras) return true;
  for (const String& name : user.cameraNames) {
    if (name.equalsIgnoreCase(cameraName)) return true;
  }
  return false;
}

static String serializeUser(const TelegramUser& u) {
  String cameras;
  for (size_t i = 0; i < u.cameraNames.size(); i++) {
    if (i > 0) cameras += LIST_SEP;
    cameras += stripSeparators(u.cameraNames[i]);
  }

  String s;
  s += stripSeparators(u.name);      s += FIELD_SEP;
  s += stripSeparators(u.chatId);    s += FIELD_SEP;
  s += (u.allCameras ? "1" : "0");   s += FIELD_SEP;
  s += cameras;                      s += FIELD_SEP;
  s += (u.systemMessages ? "1" : "0"); s += FIELD_SEP;
  s += (u.canCommand ? "1" : "0");
  return s;
}

static TelegramUser deserializeUser(const String& record) {
  TelegramUser u;
  std::vector<String> fields;
  int fieldStart = 0;
  for (int i = 0; i <= (int)record.length(); i++) {
    if (i == (int)record.length() || record[i] == FIELD_SEP) {
      fields.push_back(record.substring(fieldStart, i));
      fieldStart = i + 1;
    }
  }
  if (fields.size() < 6) return u; // malformed - caller skips entries with an empty name

  u.name           = fields[0];
  u.chatId         = fields[1];
  u.allCameras     = fields[2] == "1";
  u.systemMessages = fields[4] == "1";
  u.canCommand     = fields[5] == "1";

  String camerasField = fields[3];
  int start = 0;
  for (int i = 0; i <= (int)camerasField.length(); i++) {
    if (i == (int)camerasField.length() || camerasField[i] == LIST_SEP) {
      if (i > start) u.cameraNames.push_back(camerasField.substring(start, i));
      start = i + 1;
    }
  }
  return u;
}

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
    users.push_back(admin);
    saveTelegramUsers(users);
    Serial.println("[telegram_users] First boot with NVS-backed user storage - seeded one "
                    "\"Admin\" user from secrets.h's TELEGRAM_CHAT_ID (all cameras, system "
                    "messages, can command).");
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
