#include "config_backup.h"
#include "format_utils.h"
#include "camera_store.h"
#include "camera_serialize.h"
#include "telegram_users.h"
#include "telegram_user_serialize.h"
#include "network_store.h"
#include "network_serialize.h"
#include "sd_store.h"
#include "net_watchdog.h"
#include "bridge_watchdog.h"
#include "power_monitor.h"
#include "config_import_parse.h"
#include "nvs_chunk.h" // splitIntoChunks/joinChunks - see saveConfigBackup/loadConfigBackup
#include "build_version.h" // FIRMWARE_VERSION
#include <Preferences.h>
#include <vector>

// snapshotUriOverride is free text and may hold literal credentials
// ("http://admin:pass@host/..."); strip userinfo from the readable section. An
// '@' after the host (in the path) is left alone.
static String redactUrlCredentials(const String& url) {
  int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0) return url;
  int authorityStart = schemeEnd + 3;
  int at = url.indexOf('@', authorityStart);
  if (at < 0) return url;
  int nextSlash = url.indexOf('/', authorityStart);
  if (nextSlash >= 0 && at > nextSlash) return url; // '@' is in the path, not credentials
  return url.substring(0, authorityStart) + "(redacted)@" + url.substring(at + 1);
}

// cameraA/cameraB are free text; strip the field separator so it can't break
// the line's fields.
static String stripFieldSep(const String& s) {
  String out = s;
  out.replace(String((char)0x1F), "");
  return out;
}

// One-slot pre-import backup ("undo my last import", not a history), chunked
// like the record lists since a full export can exceed one NVS entry.
static const char* BACKUP_NVS_NAMESPACE = "cfgbackup";
static const char* BACKUP_KEY_CHUNK_COUNT = "count";
static const size_t BACKUP_CHUNK_MAX_BYTES = 1500;

static String backupChunkKey(uint16_t index) {
  char key[16];
  snprintf(key, sizeof(key), "c%u", (unsigned)index);
  return String(key);
}

static bool saveConfigBackup(const String& text) {
  std::vector<String> chunks = splitIntoChunks(text, BACKUP_CHUNK_MAX_BYTES);

  Preferences prefs;
  if (!prefs.begin(BACKUP_NVS_NAMESPACE, false)) return false;

  bool chunksOk = true;
  for (size_t i = 0; i < chunks.size(); i++) {
    if (prefs.putString(backupChunkKey((uint16_t)i).c_str(), chunks[i]) == 0) chunksOk = false;
  }
  // Remove leftover chunks from a larger previous backup.
  uint16_t oldChunkCount = prefs.getUShort(BACKUP_KEY_CHUNK_COUNT, 0);
  for (uint16_t i = (uint16_t)chunks.size(); i < oldChunkCount; i++) prefs.remove(backupChunkKey(i).c_str());

  bool countOk = prefs.putUShort(BACKUP_KEY_CHUNK_COUNT, (uint16_t)chunks.size()) > 0;
  prefs.end();
  return chunksOk && countOk;
}

String loadConfigBackup() {
  Preferences prefs;
  // Read-write (see loadDashboardAuth); this namespace doesn't exist until the
  // first import.
  prefs.begin(BACKUP_NVS_NAMESPACE, false);
  uint16_t chunkCount = prefs.getUShort(BACKUP_KEY_CHUNK_COUNT, 0);
  std::vector<String> chunks;
  chunks.reserve(chunkCount);
  for (uint16_t i = 0; i < chunkCount; i++) chunks.push_back(prefs.getString(backupChunkKey(i).c_str(), ""));
  prefs.end();
  return joinChunks(chunks);
}

ConfigImportApplyResult applyConfigImport(const String& text) {
  ConfigImportResult parsed = parseConfigImport(text);
  ConfigImportApplyResult result;

  result.anyDomainFound =
      parsed.camerasFound || parsed.usersFound || parsed.networkFound || parsed.sdSettingsFound ||
      parsed.netWatchdogFound || parsed.bridgeWatchdogFound || parsed.powerMonitorFound;
  if (result.anyDomainFound) {
    // Must snapshot before anything below writes to NVS.
    result.backupSaved = saveConfigBackup(buildConfigExport());
    if (!result.backupSaved) {
      Serial.println("[config_backup] WARNING: failed to save the pre-import backup (NVS write "
                      "error) - proceeding with the import anyway, but there will be nothing to "
                      "restore from if this goes wrong.");
    }
  }

  if (parsed.camerasFound) {
    if (parsed.camerasDuplicateName) {
      // Duplicate names: the parser left cameras empty.
      result.camerasRejectedDuplicate = true;
    } else if (replaceAllCameras(parsed.cameras)) {
      result.camerasImported = true;
      result.cameraCount = parsed.cameras.size();
    }
  }
  if (parsed.usersFound) {
    if (parsed.usersDuplicateIdentity) {
      result.usersRejectedDuplicate = true;
    } else if (replaceAllTelegramUsers(parsed.users)) {
      result.usersImported = true;
      result.userCount = parsed.users.size();
    }
  }
  if (parsed.networkFound) {
    result.networkImported = saveWifiCredentials(parsed.network);
  }
  if (parsed.sdSettingsFound) {
    result.sdSettingsImported = saveSdSettings(parsed.sdSettings);
  }
  if (parsed.netWatchdogFound) {
    result.netWatchdogImported = saveNetWatchdogSettings(parsed.netWatchdogSettings);
  }
  if (parsed.bridgeWatchdogFound) {
    result.bridgeWatchdogImported = saveBridgeWatchdogSettings(parsed.bridgeWatchdogSettings);
  }
  if (parsed.powerMonitorFound) {
    result.powerMonitorImported = savePowerMonitorSettings(parsed.powerMonitorSettings);
  }
  return result;
}

String buildConfigExport() {
  std::vector<CameraConfig> cams = loadCameras();
  std::vector<TelegramUser> users = loadTelegramUsers();
  WifiCredentials net = loadWifiCredentials();

  String out;
  out += "=== Camera Monitor v" + String(FIRMWARE_VERSION) + " Configuration Export ===\n";
  out += "Firmware build: " + String(__DATE__) + " " + String(__TIME__) + "\n";
  out += "Board uptime at export: " + formatUptime(millis()) + "\n\n";
  out += "KEEP THIS FILE PRIVATE: the machine-readable CAMERAS block below contains every\n";
  out += "camera's username and password, so an import restores them. WiFi passwords are\n";
  out += "never exported - re-enter the WiFi password after importing a Network section.\n";
  out += "\nEach section below is followed by a machine-readable block (marked '### ...') used\n";
  out += "by the Security page's Import - its lines contain non-printable field separators, so\n";
  out += "they'll look like run-together text in a plain text editor; that's expected, don't\n";
  out += "edit them by hand.\n";

  out += "\n--- Cameras (" + String(cams.size()) + ") ---\n";
  for (size_t i = 0; i < cams.size(); i++) {
    const CameraConfig& c = cams[i];
    out += "[" + String(i + 1) + "] " + c.name + "\n";
    out += "    Device service URL: " + c.deviceServiceUrl + "\n";
    out += "    Enabled: " + String(c.enabled ? "yes" : "no") + "\n";
    out += "    Username: " + c.user + "\n";
    out += "    Password: (not exported - re-enter manually)\n";
    out += "    WS-Security: " + String(c.useWSSecurity ? "yes" : "no") + "\n";
    out += "    Include InitialTerminationTime: " + String(c.includeInitialTerminationTime ? "yes" : "no") + "\n";
    out += "    Include ReplyTo anonymous: " + String(c.includeReplyToAnonymous ? "yes" : "no") + "\n";
    out += "    Snapshot URI override: " +
           (c.snapshotUriOverride.length() > 0 ? redactUrlCredentials(c.snapshotUriOverride)
                                                : String("(none)")) + "\n";
    out += "    Preferred profile keyword: " +
           (c.preferredProfileKeyword.length() > 0 ? c.preferredProfileKeyword : String("(none)")) + "\n";
    out += "    Alert cooldown: " + String(c.alertCooldownMs / 1000UL) + "s\n";
    out += "    Offline threshold: " + String(c.offlineThresholdMs / 60000UL) + "min\n";
    out += "    Snapshot burst count: " + String(c.snapshotBurstCount) + "\n";
    out += "    Notes: " + (c.notes.length() > 0 ? c.notes : String("(none)")) + "\n";
  }
  out += "### CAMERAS v" + String(CAMERA_SCHEMA_VERSION) + "\n";
  for (auto& c : cams) out += serializeCamera(c) + "\n";

  out += "\n--- Telegram Users (" + String(users.size()) + ") ---\n";
  for (size_t i = 0; i < users.size(); i++) {
    const TelegramUser& u = users[i];
    out += "[" + String(i + 1) + "] " + u.name + "\n";
    out += "    Chat ID: " + u.chatId + "\n";
    if (u.allCameras) {
      out += "    Cameras: all (including future ones)\n";
    } else {
      String list;
      for (size_t j = 0; j < u.cameraNames.size(); j++) {
        if (j > 0) list += ", ";
        list += u.cameraNames[j];
      }
      out += "    Cameras: " + (list.length() > 0 ? list : String("(none)")) + "\n";
    }
    out += "    System messages: " + String(u.systemMessages ? "yes" : "no") + "\n";
    out += "    Can command (/on /off /status /uptime): " + String(u.canCommand ? "yes" : "no") + "\n";
    out += "    Can snap (/snap): " + String(u.canSnap ? "yes" : "no") + "\n";
    out += "    Can reset (/reset): " + String(u.canReset ? "yes" : "no") + "\n";
    out += "    Can backup (/backup): " + String(u.canBackup ? "yes" : "no") + "\n";
    out += "    Can restore (/restore): " + String(u.canRestore ? "yes" : "no") + "\n";
  }
  out += "### TELEGRAM_USERS v" + String(TELEGRAM_USER_SCHEMA_VERSION) + "\n";
  for (auto& u : users) out += serializeUser(u) + "\n";

  out += "\n--- Network ---\n";
  out += "Primary SSID: " + net.primary.ssid + "\n";
  out += "Primary password: (not exported - re-enter manually)\n";
  out += "Backup SSID: " + (net.backup.ssid.length() > 0 ? net.backup.ssid : String("(none)")) + "\n";
  if (net.backup.ssid.length() > 0) out += "Backup password: (not exported - re-enter manually)\n";
  out += "Hostname: " + net.hostname + "\n";
  out += "IP mode: " + String(net.useStaticIP ? "static" : "DHCP") + "\n";
  if (net.useStaticIP) {
    out += "  Static IP: " + net.staticIP + "\n";
    out += "  Subnet: " + net.staticSubnet + "\n";
    out += "  Gateway: " + net.staticGateway + "\n";
    out += "  DNS: " + (net.staticDNS.length() > 0 ? net.staticDNS : String("(falls back to gateway)")) + "\n";
  }
  out += "NTP server: " + net.ntpServer + "\n";
  out += "NTP resync interval: " + String(net.ntpSyncIntervalMs / 60000UL) + "min\n";
  out += "POSIX TZ (blank = UTC): " + (net.posixTz.length() > 0 ? net.posixTz : String("(blank/UTC)")) + "\n";
  out += "### NETWORK v" + String(NETWORK_SCHEMA_VERSION) + "\n";
  out += serializeNetworkConfig(net) + "\n";

  out += "\n--- Storage ---\n";
  SdSettings sdSettings = loadSdSettings();
  out += "SD card storage: " + String(sdSettings.enabled ? "enabled" : "disabled") + "\n";
  out += "SD automatic full check interval: " +
         (sdSettings.checkIntervalHours > 0 ? String(sdSettings.checkIntervalHours) + "h" : String("off")) + "\n";
  out += "SD snapshot retention: " +
         (sdSettings.retentionDays > 0 ? String(sdSettings.retentionDays) + " day(s)" : String("keep forever")) + "\n";
  // Inline format (a few primitive fields), still versioned. v2 added
  // retentionDays; the v1 parser branch stays unchanged.
  out += "### SDSETTINGS v2\n";
  out += String(sdSettings.enabled ? "1" : "0") + "\x1F" + String(sdSettings.checkIntervalHours) +
         "\x1F" + String(sdSettings.retentionDays) + "\n";

  out += "\n--- Internet Watchdog ---\n";
  NetWatchdogSettings netWd = loadNetWatchdogSettings();
  out += "Enabled: " + String(netWd.enabled ? "yes" : "no") + "\n";
  out += "Relay GPIO pin: " + String(netWd.pin) + "\n";
  out += "Active-low: " + String(netWd.activeLow ? "yes" : "no") + "\n";
  out += "Outage threshold: " + String(netWd.outageThresholdMs / 60000UL) + "min\n";
  out += "Pulse duration: " + String(netWd.pulseDurationMs / 1000UL) + "s\n";
  out += "### NETWATCHDOG v1\n";
  out += String(netWd.enabled ? "1" : "0") + "\x1F" + String(netWd.pin) + "\x1F" +
         String(netWd.activeLow ? "1" : "0") + "\x1F" + String(netWd.outageThresholdMs) + "\x1F" +
         String(netWd.pulseDurationMs) + "\n";

  out += "\n--- Camera Bridge Watchdog ---\n";
  BridgeWatchdogSettings bridgeWd = loadBridgeWatchdogSettings();
  out += "Enabled: " + String(bridgeWd.enabled ? "yes" : "no") + "\n";
  out += "Camera A: " + (bridgeWd.cameraA.length() > 0 ? bridgeWd.cameraA : String("(none)")) + "\n";
  out += "Camera B: " + (bridgeWd.cameraB.length() > 0 ? bridgeWd.cameraB : String("(none)")) + "\n";
  out += "Relay GPIO pin: " + String(bridgeWd.pin) + "\n";
  out += "Active-low: " + String(bridgeWd.activeLow ? "yes" : "no") + "\n";
  out += "Outage threshold: " + String(bridgeWd.outageThresholdMs / 60000UL) + "min\n";
  out += "Pulse duration: " + String(bridgeWd.pulseDurationMs / 1000UL) + "s\n";
  out += "### BRIDGEWATCHDOG v1\n";
  out += String(bridgeWd.enabled ? "1" : "0") + "\x1F" + stripFieldSep(bridgeWd.cameraA) + "\x1F" +
         stripFieldSep(bridgeWd.cameraB) + "\x1F" + String(bridgeWd.pin) + "\x1F" +
         String(bridgeWd.activeLow ? "1" : "0") + "\x1F" + String(bridgeWd.outageThresholdMs) + "\x1F" +
         String(bridgeWd.pulseDurationMs) + "\n";

  out += "\n--- 220V Power Monitor ---\n";
  PowerMonitorSettings powerMon = loadPowerMonitorSettings();
  out += "Enabled: " + String(powerMon.enabled ? "yes" : "no") + "\n";
  out += "Sensor GPIO pin: " + String(powerMon.pin) + "\n";
  out += "Active-high: " + String(powerMon.activeHigh ? "yes" : "no") + "\n";
  out += "### POWERMONITOR v1\n";
  out += String(powerMon.enabled ? "1" : "0") + "\x1F" + String(powerMon.pin) + "\x1F" +
         String(powerMon.activeHigh ? "1" : "0") + "\n";

  return out;
}
