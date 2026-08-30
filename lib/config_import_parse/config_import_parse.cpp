#include "config_import_parse.h"
#include <cctype>
#include <cstring>

// Same separator config_export's SDSETTINGS line uses (webserver_security.cpp) -
// only 2 fields, no realistic future reordering risk, so this stays a tiny
// inline parser rather than its own schema-versioned lib module the way
// cameras/users/network (all string-bearing) warrant.
static const char SD_FIELD_SEP = '\x1F';
static const uint16_t SDSETTINGS_SCHEMA_VERSION_KNOWN = 1;

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

// SDSETTINGS' one data line: "<enabled 0/1>\x1F<checkIntervalHours>". No
// dedicated serializer (see this file's own comment) - built/parsed here
// directly. Returns false (settings untouched) if malformed.
static bool parseSdSettingsLine(const String& line, SdSettings& out) {
  int sep = line.indexOf(SD_FIELD_SEP);
  if (sep < 0) return false;
  String enabledField = line.substring(0, sep);
  String hoursField = line.substring(sep + 1);
  if (enabledField.length() == 0) return false;
  out.enabled = enabledField == "1";
  out.checkIntervalHours = (uint32_t)hoursField.toInt();
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
    } else if (currentSection == "TELEGRAM_USERS") {
      result.usersFound = true;
      for (auto& l : sectionLines) {
        if (l.length() == 0) continue;
        TelegramUser u = deserializeUser(l, currentVersion);
        if (u.name.length() > 0) result.users.push_back(u);
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
        // Only one schema so far - kept as an explicit check (not just
        // "parse whatever's there") so a future v2 has a clear place to
        // branch, same discipline as the schema-versioned serializers.
        if (currentVersion == SDSETTINGS_SCHEMA_VERSION_KNOWN && parseSdSettingsLine(l, s)) {
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
