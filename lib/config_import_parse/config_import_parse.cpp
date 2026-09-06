#include "config_import_parse.h"
#include "telegram_parse.h" // chatIdMatches - see usersHaveDuplicateIdentity below
#include <cctype>
#include <cstring>

// See ConfigImportResult::camerasDuplicateName's own comment (config_import_parse.h)
// for why this check exists at all - O(n^2) is fine here, a config import's
// camera count is nowhere near large enough for that to matter.
static bool camerasHaveDuplicateName(const std::vector<CameraConfig>& cameras) {
  for (size_t i = 0; i < cameras.size(); i++) {
    for (size_t j = i + 1; j < cameras.size(); j++) {
      if (cameras[i].name.equalsIgnoreCase(cameras[j].name)) return true;
    }
  }
  return false;
}

// See ConfigImportResult::usersDuplicateIdentity's own comment. Chat ID
// comparison reuses chatIdMatches (telegram_parse.h) - the same numeric
// comparison telegram_users.cpp's addTelegramUser/updateTelegramUser use -
// so this can't be fooled by two chat IDs that are textually different but
// numerically identical (e.g. a stray leading zero) the way a raw String
// == compare could be.
static bool usersHaveDuplicateIdentity(const std::vector<TelegramUser>& users) {
  for (size_t i = 0; i < users.size(); i++) {
    int64_t chatIdI = strtoll(users[i].chatId.c_str(), nullptr, 10);
    for (size_t j = i + 1; j < users.size(); j++) {
      if (users[i].name.equalsIgnoreCase(users[j].name)) return true;
      if (chatIdMatches(users[j].chatId, chatIdI)) return true;
    }
  }
  return false;
}

// Same separator config_export's SDSETTINGS line uses (webserver_security.cpp) -
// few enough primitive fields that this stays a tiny inline parser rather
// than its own schema-versioned lib module the way cameras/users/network
// (all string-bearing) warrant - but still version-tagged, same discipline,
// since "no realistic future reordering risk" already turned out wrong
// once (retentionDays was added to SdSettings after this format existed,
// silently dropped by every import until v2 below).
static const char SD_FIELD_SEP = '\x1F';
static const uint16_t SDSETTINGS_SCHEMA_VERSION_CURRENT = 2;

static std::vector<String> splitLines(const String& text) {
  std::vector<String> lines;
  int start = 0;
  for (int i = 0; i <= (int)text.length(); i++) {
    if (i == (int)text.length() || text[i] == '\n') {
      String line = text.substring(start, i);
      if (line.endsWith("\r")) line.remove(line.length() - 1); // tolerate CRLF exports
      lines.push_back(line);
      start = i + 1;
    }
  }
  return lines;
}

struct SectionMarker {
  bool ok = false;
  String section;
  uint16_t version = 0;
};

// "### <SECTION> v<N>" - the only shape this ever looks for. Deliberately
// NOT dependent on anything else in the file (prose wording elsewhere has
// already been reworded more than once in this project's history).
static SectionMarker parseMarkerLine(const String& line) {
  SectionMarker m;
  static const char* kPrefix = "### ";
  if (!line.startsWith(kPrefix)) return m;

  String rest = line.substring(strlen(kPrefix));
  int vPos = rest.lastIndexOf(" v");
  if (vPos < 0) return m;

  String versionStr = rest.substring(vPos + 2);
  versionStr.trim();
  if (versionStr.length() == 0) return m;
  for (size_t i = 0; i < versionStr.length(); i++) {
    if (!isdigit((unsigned char)versionStr[i])) return m;
  }

  m.section = rest.substring(0, vPos);
  m.version = (uint16_t)versionStr.toInt();
  m.ok = true;
  return m;
}

// Version 1 (superseded): "<enabled 0/1>\x1F<checkIntervalHours>" -
// retentionDays didn't exist on SdSettings yet when this line format was
// first written. Kept exactly as it was, never edited, so an export taken
// before retention existed still imports correctly - out.retentionDays
// simply stays at SdSettings' own struct default (SD_RETENTION_DAYS_DEFAULT)
// for a line in this shape, same "old record, current default for the
// field it doesn't mention" pattern camera_serialize.cpp's V0 branch uses.
static bool parseSdSettingsLineV1(const String& line, SdSettings& out) {
  int sep = line.indexOf(SD_FIELD_SEP);
  if (sep < 0) return false;
  String enabledField = line.substring(0, sep);
  String hoursField = line.substring(sep + 1);
  if (enabledField.length() == 0) return false;
  out.enabled = enabledField == "1";
  out.checkIntervalHours = (uint32_t)hoursField.toInt();
  return true;
}

// Version 2 (SDSETTINGS_SCHEMA_VERSION_CURRENT): V1's 2 fields plus
// retentionDays, appended - "<enabled 0/1>\x1F<checkIntervalHours>\x1F<retentionDays>".
static bool parseSdSettingsLineV2(const String& line, SdSettings& out) {
  int sep1 = line.indexOf(SD_FIELD_SEP);
  if (sep1 < 0) return false;
  int sep2 = line.indexOf(SD_FIELD_SEP, sep1 + 1);
  if (sep2 < 0) return false;
  String enabledField = line.substring(0, sep1);
  String hoursField = line.substring(sep1 + 1, sep2);
  String retentionField = line.substring(sep2 + 1);
  if (enabledField.length() == 0) return false;
  out.enabled = enabledField == "1";
  out.checkIntervalHours = (uint32_t)hoursField.toInt();
  if (retentionField.length() > 0) out.retentionDays = (uint16_t)retentionField.toInt();
  return true;
}

ConfigImportResult parseConfigImport(const String& text) {
  ConfigImportResult result;
  std::vector<String> lines = splitLines(text);

  String currentSection;
  uint16_t currentVersion = 0;
  bool inSection = false;
  std::vector<String> sectionLines;

  auto flushSection = [&]() {
    if (!inSection) return;

    if (currentSection == "CAMERAS") {
      result.camerasFound = true;
      for (auto& l : sectionLines) {
        if (l.length() == 0) continue;
        CameraConfig c = deserializeCamera(l, currentVersion);
        if (c.name.length() > 0) result.cameras.push_back(c);
      }
      if (camerasHaveDuplicateName(result.cameras)) {
        result.camerasDuplicateName = true;
        result.cameras.clear();
      }
    } else if (currentSection == "TELEGRAM_USERS") {
      result.usersFound = true;
      for (auto& l : sectionLines) {
        if (l.length() == 0) continue;
        TelegramUser u = deserializeUser(l, currentVersion);
        if (u.name.length() > 0) result.users.push_back(u);
      }
      if (usersHaveDuplicateIdentity(result.users)) {
        result.usersDuplicateIdentity = true;
        result.users.clear();
      }
    } else if (currentSection == "NETWORK") {
      for (auto& l : sectionLines) {
        if (l.length() == 0) continue;
        WifiCredentials creds = deserializeNetworkConfig(l, currentVersion);
        if (creds.hostname.length() > 0) {
          result.network = creds;
          result.networkFound = true;
        }
        break; // exactly one data line expected
      }
    } else if (currentSection == "SDSETTINGS") {
      for (auto& l : sectionLines) {
        if (l.length() == 0) continue;
        SdSettings s;
        bool parsed = false;
        if (currentVersion == 1) parsed = parseSdSettingsLineV1(l, s);
        else if (currentVersion == SDSETTINGS_SCHEMA_VERSION_CURRENT) parsed = parseSdSettingsLineV2(l, s);
        if (parsed) {
          result.sdSettings = s;
          result.sdSettingsFound = true;
        }
        break; // exactly one data line expected
      }
    }
    sectionLines.clear();
  };

  for (auto& line : lines) {
    SectionMarker m = parseMarkerLine(line);
    if (m.ok) {
      flushSection();
      currentSection = m.section;
      currentVersion = m.version;
      inSection = true;
      continue;
    }
    if (inSection) sectionLines.push_back(line);
  }
  flushSection();

  return result;
}
