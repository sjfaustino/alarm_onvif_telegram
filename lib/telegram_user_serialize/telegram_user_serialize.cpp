#include "telegram_user_serialize.h"
#include "record_table.h"

bool telegramUserWantsCamera(const TelegramUser& user, const String& cameraName) {
  if (user.allCameras) return true;
  for (const String& name : user.cameraNames) {
    if (name.equalsIgnoreCase(cameraName)) return true;
  }
  return false;
}

// LIST_SEP separates the camera names inside one field.
static const char LIST_SEP = '\x1D';

static String stripSeparators(const String& s) {
  String out = stripFieldSeparator(s);
  out.replace(String(LIST_SEP), "");
  return out;
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

static void writeCameraNames(const TelegramUser& u, String& out) {
  for (size_t i = 0; i < u.cameraNames.size(); i++) {
    if (i > 0) out += LIST_SEP;
    out += stripSeparators(u.cameraNames[i]);
  }
}
static void readCameraNames(TelegramUser& u, const String& v) { u.cameraNames = splitCameraList(v); }

static void writeLanguage(const TelegramUser& u, String& out) { out += String((int)u.language); }
static void readLanguage(TelegramUser& u, const String& v) {
  u.language = v.toInt() == 1 ? TelegramLang::Portuguese : TelegramLang::English;
}

using U = TelegramUser;

// The record layout, in order; see record_table.h. canSnap was only ever
// appended to version 0, so that version may stop one row short.
static const RecordField<TelegramUser> kFields[] = {
    recordText<&U::name, stripSeparators>(0),
    recordText<&U::chatId, stripSeparators>(0),
    recordFlag<&U::allCameras>(0),
    {0, writeCameraNames, readCameraNames},
    recordFlag<&U::systemMessages>(0),
    recordFlag<&U::canCommand>(0),
    recordFlag<&U::canSnap>(0),
    recordFlag<&U::canReset>(2),
    recordNumber<&U::maxCommandsPerMinute>(3),
    {4, writeLanguage, readLanguage},
    recordFlag<&U::canBackup>(5),
    recordFlag<&U::canRestore>(6),
};
static const size_t kV0MinFields = 6;

String serializeUser(const TelegramUser& u) { return writeRecord(kFields, u); }

TelegramUser deserializeUser(const String& record, uint16_t recordVersion) {
  return readRecord(kFields, record, recordVersion, kV0MinFields); // the loader warns if newer
}

size_t telegramUserRecordFieldCount(const String& record) {
  return splitRecordFields(record).size();
}
