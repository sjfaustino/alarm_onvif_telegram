#include "telegram_user_serialize.h"

bool telegramUserWantsCamera(const TelegramUser& user, const String& cameraName) {
  if (user.allCameras) return true;
  for (const String& name : user.cameraNames) {
    if (name.equalsIgnoreCase(cameraName)) return true;
  }
  return false;
}

// Same scheme as camera_serialize.cpp: FIELD_SEP between fields, LIST_SEP
// between the camera names inside one field.
static const char FIELD_SEP = '\x1F';
static const char LIST_SEP  = '\x1D';

static String stripSeparators(const String& s) {
  String out = s;
  out.replace(String(FIELD_SEP), "");
  out.replace(String(LIST_SEP), "");
  return out;
}

static std::vector<String> splitFields(const String& record) {
  std::vector<String> fields;
  int fieldStart = 0;
  for (int i = 0; i <= (int)record.length(); i++) {
    if (i == (int)record.length() || record[i] == FIELD_SEP) {
      fields.push_back(record.substring(fieldStart, i));
      fieldStart = i + 1;
    }
  }
  return fields;
}

static std::vector<String> splitCameraList(const String& camerasField) {
  std::vector<String> names;
  int start = 0;
  for (int i = 0; i <= (int)camerasField.length(); i++) {
    if (i == (int)camerasField.length() || camerasField[i] == LIST_SEP) {
      if (i > start) names.push_back(camerasField.substring(start, i));
      start = i + 1;
    }
  }
  return names;
}

String serializeUser(const TelegramUser& u) {
  String cameras;
  for (size_t i = 0; i < u.cameraNames.size(); i++) {
    if (i > 0) cameras += LIST_SEP;
    cameras += stripSeparators(u.cameraNames[i]);
  }

  String s;
  s += stripSeparators(u.name);        s += FIELD_SEP;
  s += stripSeparators(u.chatId);      s += FIELD_SEP;
  s += (u.allCameras ? "1" : "0");     s += FIELD_SEP;
  s += cameras;                        s += FIELD_SEP;
  s += (u.systemMessages ? "1" : "0"); s += FIELD_SEP;
  s += (u.canCommand ? "1" : "0");     s += FIELD_SEP;
  s += (u.canSnap ? "1" : "0");        s += FIELD_SEP;
  s += (u.canReset ? "1" : "0");       s += FIELD_SEP;
  s += String(u.maxCommandsPerMinute); s += FIELD_SEP;
  s += String((int)u.language);        s += FIELD_SEP;
  s += (u.canBackup ? "1" : "0");       s += FIELD_SEP;
  s += (u.canRestore ? "1" : "0");
  return s;
}

// V0 (before versioning): 6 or 7 fields - canSnap was only ever appended.
static TelegramUser deserializeUserV0(const std::vector<String>& fields) {
  TelegramUser u;
  if (fields.size() < 6) return u; // malformed - caller skips entries with an empty name

  u.name           = fields[0];
  u.chatId         = fields[1];
  u.allCameras     = fields[2] == "1";
  u.systemMessages = fields[4] == "1";
  u.canCommand     = fields[5] == "1";
  if (fields.size() >= 7) {
    u.canSnap = fields[6] == "1";
  }
  u.cameraNames = splitCameraList(fields[3]);
  return u;
}

// Old version branches are never edited, and V1 on require an exact field
// count. V1: 7 fields.
static TelegramUser deserializeUserV1(const std::vector<String>& fields) {
  TelegramUser u;
  if (fields.size() != 7) return u; // malformed - caller skips entries with an empty name

  u.name           = fields[0];
  u.chatId         = fields[1];
  u.allCameras     = fields[2] == "1";
  u.systemMessages = fields[4] == "1";
  u.canCommand     = fields[5] == "1";
  u.canSnap        = fields[6] == "1";
  u.cameraNames    = splitCameraList(fields[3]);
  return u;
}

// V2: + canReset.
static TelegramUser deserializeUserV2(const std::vector<String>& fields) {
  TelegramUser u;
  if (fields.size() != 8) return u; // malformed - caller skips entries with an empty name

  u.name           = fields[0];
  u.chatId         = fields[1];
  u.allCameras     = fields[2] == "1";
  u.systemMessages = fields[4] == "1";
  u.canCommand     = fields[5] == "1";
  u.canSnap        = fields[6] == "1";
  u.canReset       = fields[7] == "1";
  u.cameraNames    = splitCameraList(fields[3]);
  return u;
}

// V3: + maxCommandsPerMinute.
static TelegramUser deserializeUserV3(const std::vector<String>& fields) {
  TelegramUser u;
  if (fields.size() != 9) return u; // malformed - caller skips entries with an empty name

  u.name           = fields[0];
  u.chatId         = fields[1];
  u.allCameras     = fields[2] == "1";
  u.systemMessages = fields[4] == "1";
  u.canCommand     = fields[5] == "1";
  u.canSnap        = fields[6] == "1";
  u.canReset       = fields[7] == "1";
  if (fields[8].length() > 0) u.maxCommandsPerMinute = (uint16_t)fields[8].toInt();
  u.cameraNames    = splitCameraList(fields[3]);
  return u;
}

// V4: + language.
static TelegramUser deserializeUserV4(const std::vector<String>& fields) {
  TelegramUser u;
  if (fields.size() != 10) return u; // malformed - caller skips entries with an empty name

  u.name           = fields[0];
  u.chatId         = fields[1];
  u.allCameras     = fields[2] == "1";
  u.systemMessages = fields[4] == "1";
  u.canCommand     = fields[5] == "1";
  u.canSnap        = fields[6] == "1";
  u.canReset       = fields[7] == "1";
  if (fields[8].length() > 0) u.maxCommandsPerMinute = (uint16_t)fields[8].toInt();
  u.language       = fields[9].toInt() == 1 ? TelegramLang::Portuguese : TelegramLang::English;
  u.cameraNames    = splitCameraList(fields[3]);
  return u;
}

// V5: + canBackup.
static TelegramUser deserializeUserV5(const std::vector<String>& fields) {
  TelegramUser u;
  if (fields.size() != 11) return u; // malformed - caller skips entries with an empty name

  u.name           = fields[0];
  u.chatId         = fields[1];
  u.allCameras     = fields[2] == "1";
  u.systemMessages = fields[4] == "1";
  u.canCommand     = fields[5] == "1";
  u.canSnap        = fields[6] == "1";
  u.canReset       = fields[7] == "1";
  if (fields[8].length() > 0) u.maxCommandsPerMinute = (uint16_t)fields[8].toInt();
  u.language       = fields[9].toInt() == 1 ? TelegramLang::Portuguese : TelegramLang::English;
  u.canBackup      = fields[10] == "1";
  u.cameraNames    = splitCameraList(fields[3]);
  return u;
}

// V6: + canRestore.
static TelegramUser deserializeUserV6(const std::vector<String>& fields) {
  TelegramUser u;
  if (fields.size() != 12) return u; // malformed - caller skips entries with an empty name

  u.name           = fields[0];
  u.chatId         = fields[1];
  u.allCameras     = fields[2] == "1";
  u.systemMessages = fields[4] == "1";
  u.canCommand     = fields[5] == "1";
  u.canSnap        = fields[6] == "1";
  u.canReset       = fields[7] == "1";
  if (fields[8].length() > 0) u.maxCommandsPerMinute = (uint16_t)fields[8].toInt();
  u.language       = fields[9].toInt() == 1 ? TelegramLang::Portuguese : TelegramLang::English;
  u.canBackup      = fields[10] == "1";
  u.canRestore     = fields[11] == "1";
  u.cameraNames    = splitCameraList(fields[3]);
  return u;
}

TelegramUser deserializeUser(const String& record, uint16_t recordVersion) {
  std::vector<String> fields = splitFields(record);

  if (recordVersion == 0) return deserializeUserV0(fields);
  if (recordVersion == 1) return deserializeUserV1(fields);
  if (recordVersion == 2) return deserializeUserV2(fields);
  if (recordVersion == 3) return deserializeUserV3(fields);
  if (recordVersion == 4) return deserializeUserV4(fields);
  if (recordVersion == 5) return deserializeUserV5(fields);
  if (recordVersion == TELEGRAM_USER_SCHEMA_VERSION) return deserializeUserV6(fields);

  // Unknown/newer version: try the newest layout (loadTelegramUsers warns).
  return deserializeUserV6(fields);
}

size_t telegramUserRecordFieldCount(const String& record) {
  return splitFields(record).size();
}
