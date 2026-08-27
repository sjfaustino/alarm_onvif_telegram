#include "telegram_user_serialize.h"

bool telegramUserWantsCamera(const TelegramUser& user, const String& cameraName) {
  if (user.allCameras) return true;
  for (const String& name : user.cameraNames) {
    if (name.equalsIgnoreCase(cameraName)) return true;
  }
  return false;
}

// Same non-printable ASCII separator scheme as camera_serialize.cpp - no
// user name, chat id, or camera name should ever legitimately contain
// these. FIELD_SEP separates one user's fields; LIST_SEP separates the
// camera names packed into the "cameras" field. Not the same as
// telegram_users.cpp's own RECORD_SEP (separates whole user records),
// which is private to that file.
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

// Always emits TELEGRAM_USER_SCHEMA_VERSION's layout.
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
  s += (u.canSnap ? "1" : "0");
  return s;
}

// Version 0: pre-versioning format. Tolerant of the trailing canSnap
// field being absent (6 fields, in addition to the full 7) because it was,
// as a matter of historical fact, only ever appended - never inserted or
// reordered. See camera_serialize.cpp's deserializeCameraV0 for why that
// assumption is safe *only* for genuinely historical data, not something
// to lean on again going forward.
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

// Version 1 (TELEGRAM_USER_SCHEMA_VERSION): the current, fixed 7-field
// layout. Requires an exact field count - see camera_serialize.cpp's
// deserializeCameraV1 for the same reasoning.
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

TelegramUser deserializeUser(const String& record, uint16_t recordVersion) {
  std::vector<String> fields = splitFields(record);

  if (recordVersion == 0) return deserializeUserV0(fields);
  if (recordVersion == TELEGRAM_USER_SCHEMA_VERSION) return deserializeUserV1(fields);

  // Unknown/future version - best-effort fall through to the newest known
  // layout; telegram_users.cpp logs a warning when this happens.
  return deserializeUserV1(fields);
}
