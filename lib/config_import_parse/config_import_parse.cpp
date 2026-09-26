#include "config_import_parse.h"
#include "telegram_parse.h" // chatIdMatches - see usersHaveDuplicateIdentity below
#include <cctype>
#include <cstring>

// O(n^2) is fine at these sizes.
static bool camerasHaveDuplicateName(const std::vector<CameraConfig>& cameras) {
  for (size_t i = 0; i < cameras.size(); i++) {
    for (size_t j = i + 1; j < cameras.size(); j++) {
      if (cameras[i].name.equalsIgnoreCase(cameras[j].name)) return true;
    }
  }
  return false;
}

// chatIdMatches compares numerically, so "0123" and "123" collide.
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

// Inline parsing for the small primitive sections, still versioned: SD
// retentionDays was once silently dropped by every import until v2.
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

// Depends only on the marker line, never on prose wording.
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

// v1: "<enabled>\x1F<checkIntervalHours>"; retentionDays keeps its default.
// Never edited.
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

// v2: v1 + "\x1F<retentionDays>".
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

static std::vector<String> splitFieldsBySep(const String& line, char sep) {
  std::vector<String> fields;
  int start = 0;
  for (int i = 0; i <= (int)line.length(); i++) {
    if (i == (int)line.length() || line[i] == sep) {
      fields.push_back(line.substring(start, i));
      start = i + 1;
    }
  }
  return fields;
}

// v1:
// "<enabled>\x1F<pin>\x1F<activeLow>\x1F<outageThresholdMs>\x1F<pulseDurationMs>".
static bool parseNetWatchdogLineV1(const String& line, NetWatchdogSettings& out) {
  std::vector<String> f = splitFieldsBySep(line, SD_FIELD_SEP);
  if (f.size() != 5 || f[0].length() == 0) return false;
  out.enabled = f[0] == "1";
  out.pin = f[1].toInt();
  out.activeLow = f[2] == "1";
  out.outageThresholdMs = (uint32_t)f[3].toInt();
  out.pulseDurationMs = (uint32_t)f[4].toInt();
  return true;
}

// v1:
// "<enabled>\x1F<cameraA>\x1F<cameraB>\x1F<pin>\x1F<activeLow>\x1F<outageThresholdMs>\x1F<pulseDurationMs>".
static bool parseBridgeWatchdogLineV1(const String& line, BridgeWatchdogSettings& out) {
  std::vector<String> f = splitFieldsBySep(line, SD_FIELD_SEP);
  if (f.size() != 7 || f[0].length() == 0) return false;
  out.enabled = f[0] == "1";
  out.cameraA = f[1];
  out.cameraB = f[2];
  out.pin = f[3].toInt();
  out.activeLow = f[4] == "1";
  out.outageThresholdMs = (uint32_t)f[5].toInt();
  out.pulseDurationMs = (uint32_t)f[6].toInt();
  return true;
}

// v1: "<enabled>\x1F<pin>\x1F<activeHigh>".
static bool parsePowerMonitorLineV1(const String& line, PowerMonitorSettings& out) {
  std::vector<String> f = splitFieldsBySep(line, SD_FIELD_SEP);
  if (f.size() != 3 || f[0].length() == 0) return false;
  out.enabled = f[0] == "1";
  out.pin = f[1].toInt();
  out.activeHigh = f[2] == "1";
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
    } else if (currentSection == "NETWATCHDOG") {
      for (auto& l : sectionLines) {
        if (l.length() == 0) continue;
        NetWatchdogSettings s;
        if (currentVersion == 1 && parseNetWatchdogLineV1(l, s)) {
          result.netWatchdogSettings = s;
          result.netWatchdogFound = true;
        }
        break; // exactly one data line expected
      }
    } else if (currentSection == "BRIDGEWATCHDOG") {
      for (auto& l : sectionLines) {
        if (l.length() == 0) continue;
        BridgeWatchdogSettings s;
        if (currentVersion == 1 && parseBridgeWatchdogLineV1(l, s)) {
          result.bridgeWatchdogSettings = s;
          result.bridgeWatchdogFound = true;
        }
        break; // exactly one data line expected
      }
    } else if (currentSection == "POWERMONITOR") {
      for (auto& l : sectionLines) {
        if (l.length() == 0) continue;
        PowerMonitorSettings s;
        if (currentVersion == 1 && parsePowerMonitorLineV1(l, s)) {
          result.powerMonitorSettings = s;
          result.powerMonitorFound = true;
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
