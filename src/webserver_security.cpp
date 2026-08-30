#include "webserver_security.h"
#include "auth_store.h"
#include "format_utils.h"
#include "camera_store.h"
#include "camera_serialize.h"
#include "telegram_users.h"
#include "telegram_user_serialize.h"
#include "network_store.h"
#include "network_serialize.h"
#include "sd_store.h"
#include "config_import_parse.h"
#include "nvs_chunk.h" // splitIntoChunks/joinChunks - see saveConfigBackup/loadConfigBackup
#include "build_version.h" // FIRMWARE_VERSION
#include <Preferences.h>

// snapshotUriOverride (buildConfigExport, below) is free text, not
// necessarily using the intended {USER}/{PASS} placeholder pattern - a
// user could instead type literal embedded URL credentials
// ("http://admin:realpass@host/snap.jpg"). This function's own stated
// guarantee ("Password: (not exported...)") only actually holds if that
// case is caught too, not just the placeholder path - strips any
// "user[:pass]@" authority-section credentials before export. A bare '@'
// that's actually part of the URL's path (not the authority section, i.e.
// appears after the first '/' following the scheme) is left untouched.
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

String renderSecurityPanel() {
  DashboardAuth auth = loadDashboardAuth();
  bool configured = auth.username.length() > 0 && auth.password.length() > 0;

  String html = "<h1>Security</h1>";
  if (configured) {
    html += "<p class=\"hint\">A dashboard login is set (username: " + htmlEscape(auth.username) +
            "). Every page here, including this one and the Firmware upload, now requires it. "
            "Changing it below takes effect on your very next request.</p>";
  } else {
    html += "<p class=\"hint\">No login is set - this dashboard, including the Firmware upload page, "
            "is reachable by anyone on your LAN with no password. Set one below to require it on "
            "every page from now on.</p>";
  }

  html += "<fieldset><legend>" + String(configured ? "Change" : "Set") + " dashboard login</legend>";
  html += "<form method=\"POST\" action=\"/security/save\">";
  html += "<label>Username<input type=\"text\" name=\"username\" value=\"" + htmlEscape(auth.username) +
          "\" required></label>";
  html += "<label>Password<input type=\"password\" name=\"password\" required></label>";
  html += "<label>Confirm password<input type=\"password\" name=\"confirmPassword\" required></label>";
  html += "<p><button type=\"submit\">Save</button></p></form></fieldset>";

  html += "<p class=\"hint\">There's no recovery flow if this is lost - forgetting it means erasing "
          "the board's NVS entirely (wiping cameras, WiFi, and Telegram users too, not just this) "
          "to get back in. Keep it somewhere safe.</p>";

  html += "<fieldset><legend>Backup</legend>";
  html += "<p class=\"hint\">Downloads every camera, Telegram user, and network setting currently "
          "stored, as plain text - not the passwords (camera or WiFi), which aren't exported and "
          "have to be re-entered manually. Useful to have on hand if NVS is ever erased (see above) "
          "or a board gets replaced - reconstructing everything else from memory is the tedious part.</p>";
  html += "<p><a href=\"/export\"><button type=\"button\">Export configuration</button></a></p>";
  html += "</fieldset>";

  html += "<fieldset><legend>Import</legend>";
  html += "<p class=\"hint\">Restores cameras, Telegram users, network settings, and SD settings "
          "from a previously exported configuration file - REPLACES whichever of those sections "
          "the file actually contains (a section missing from the file is left untouched). "
          "Passwords are never in an export, so imported cameras/network will need theirs "
          "re-entered before they'll work - if Network is included, that means the WiFi password "
          "too: rebooting before fixing it strands the board off the network entirely, reachable "
          "only via physical/serial access. Takes effect after a reboot, same as any other "
          "camera/network change. Only files exported by this Import feature (this build or "
          "later) can be restored - older exports have nothing for it to read. Every import "
          "automatically saves a backup of what was stored just before it - if the wrong file "
          "gets imported, <a href=\"/import/backup\">download that backup</a> and import it "
          "again to undo.</p>";
  // multipart/form-data, not a plain form field - a config file grows
  // roughly linearly with camera/user count, and the URL-encoded plain-
  // form approach this used to use (the browser percent-encoding every
  // newline and every \x1F machine-readable-block separator as 3 bytes
  // each) could blow well past the dashboard's ordinary request-body cap
  // on exactly the multi-camera setups this feature exists to back up. A
  // real file upload is bounded by the much larger upload-size limit
  // (webserver.cpp's importHandler) instead, and streams in rather than
  // needing to be buffered whole by the browser into a hidden field first.
  html += "<form method=\"POST\" action=\"/import\" enctype=\"multipart/form-data\" "
          "onsubmit=\"return confirm('Import this "
          "configuration? This REPLACES cameras/Telegram users/network/SD settings currently "
          "stored with whatever the file contains (a section missing from the file is left "
          "alone). Passwords will need to be re-entered - if the file includes Network, do NOT "
          "reboot until you have re-entered the WiFi password, or the board will be unable to "
          "reconnect.');\">";
  html += "<label>Configuration file (.txt from Export above)"
          "<input type=\"file\" name=\"configFile\" accept=\".txt\" required></label>";
  html += "<p><button type=\"submit\">Import configuration</button></p></form></fieldset>";
  return html;
}

// One-slot pre-import backup - see applyConfigImport's own comment for why
// this needs to exist at all. Chunked the same way camera_store.cpp/
// telegram_users.cpp chunk their own record lists (nvs_chunk.h) - a full
// export with several cameras/users can exceed NVS's practical per-entry
// size ceiling, same reasoning as their own NVS_KEY_LIST_LEGACY comments.
// Only ever one backup slot (each new import overwrites the last) - this
// is a "just before this last mistake" safety net, not a version history.
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
  // Drop leftover chunk keys from a previous, larger backup - same reasoning
  // as camera_store.cpp's saveCameras().
  uint16_t oldChunkCount = prefs.getUShort(BACKUP_KEY_CHUNK_COUNT, 0);
  for (uint16_t i = (uint16_t)chunks.size(); i < oldChunkCount; i++) prefs.remove(backupChunkKey(i).c_str());

  bool countOk = prefs.putUShort(BACKUP_KEY_CHUNK_COUNT, (uint16_t)chunks.size()) > 0;
  prefs.end();
  return chunksOk && countOk;
}

String loadConfigBackup() {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for
  // why (this namespace is never written until the first import, so a
  // read-only open would spam a NOT_FOUND error on every /import/backup
  // request - or every render, if this is ever surfaced there too - until
  // that first import happens).
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
      parsed.camerasFound || parsed.usersFound || parsed.networkFound || parsed.sdSettingsFound;
  if (result.anyDomainFound) {
    // Captures whatever is CURRENTLY in NVS before any of the replaceAll*/
    // save* calls below touch it - see this function's own header comment
    // (webserver_security.h) for why this has to happen first, not after.
    result.backupSaved = saveConfigBackup(buildConfigExport());
    if (!result.backupSaved) {
      Serial.println("[webserver_security] WARNING: failed to save the pre-import backup (NVS write "
                      "error) - proceeding with the import anyway, but there will be nothing to "
                      "restore from if this goes wrong.");
    }
  }

  if (parsed.camerasFound) {
    if (replaceAllCameras(parsed.cameras)) {
      result.camerasImported = true;
      result.cameraCount = parsed.cameras.size();
    }
  }
  if (parsed.usersFound) {
    if (replaceAllTelegramUsers(parsed.users)) {
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
  return result;
}

String renderImportResultBanner(const ConfigImportApplyResult& r) {
  if (!r.anyDomainFound) {
    return "No valid configuration sections found in this file - nothing was changed. "
           "(Only files exported by this build or later can be restored - an older export "
           "has nothing for Import to read.)";
  }

  String imported, skipped;
  auto addImported = [&](const String& s) { if (imported.length() > 0) imported += ", "; imported += s; };
  if (r.camerasImported) addImported(String(r.cameraCount) + " camera(s)");
  if (r.usersImported) addImported(String(r.userCount) + " Telegram user(s)");
  if (r.networkImported) addImported("network settings");
  if (r.sdSettingsImported) addImported("SD settings");

  auto addSkipped = [&](const String& s) { if (skipped.length() > 0) skipped += ", "; skipped += s; };
  if (!r.camerasImported) addSkipped("Cameras");
  if (!r.usersImported) addSkipped("Telegram Users");
  if (!r.networkImported) addSkipped("Network");
  if (!r.sdSettingsImported) addSkipped("SD Settings");

  String banner = imported.length() > 0 ? ("Imported " + imported + ".") : "Nothing was imported.";
  if (skipped.length() > 0) {
    banner += " " + skipped + " not found in this file (or failed to save) - left unchanged.";
  }
  if (r.networkImported) {
    // Stronger wording than the plain camera-password note below -
    // rebooting with a blank WiFi password (not just a broken camera)
    // strands the board off the network entirely, reachable only via
    // physical/serial access to fix.
    banner += " \xE2\x9A\xA0\xEF\xB8\x8F Network was imported WITHOUT a WiFi password (never "
              "included in an export) - go to the Network page and re-enter it now. Rebooting "
              "before fixing this will leave the board unable to reconnect to WiFi at all.";
  }
  if (r.camerasImported) {
    banner += " Imported camera(s) also have blank passwords - re-enter them on the Cameras "
              "page before rebooting.";
  }
  if (imported.length() > 0) {
    banner += r.backupSaved
        ? " A backup of what was stored just before this import was saved automatically - "
          "<a href=\"/import/backup\">download it</a> if you need to undo this."
        : " \xE2\x9A\xA0\xEF\xB8\x8F The automatic pre-import backup FAILED to save (NVS write "
          "error) - there is nothing to undo this with if it turns out wrong.";
    banner += " Reboot the board (Maintenance page) to apply.";
  }
  return banner;
}

String buildConfigExport() {
  std::vector<CameraConfig> cams = loadCameras();
  std::vector<TelegramUser> users = loadTelegramUsers();
  WifiCredentials net = loadWifiCredentials();

  String out;
  out += "=== Camera Monitor v" + String(FIRMWARE_VERSION) + " Configuration Export ===\n";
  out += "Firmware build: " + String(__DATE__) + " " + String(__TIME__) + "\n";
  out += "Board uptime at export: " + formatUptime(millis()) + "\n\n";
  out += "Passwords (camera and WiFi) are deliberately NOT included below - re-enter them\n";
  out += "manually after a restore. Everything else here is what's tedious to reconstruct\n";
  out += "from memory via the dashboard.\n";
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
  // No dedicated serializer - see config_import_parse.cpp's own comment on
  // why this stays a tiny inline "enabled\x1FcheckIntervalHours" line
  // rather than a whole schema-versioned lib module for 2 primitive fields.
  out += "### SDSETTINGS v1\n";
  out += String(sdSettings.enabled ? "1" : "0") + "\x1F" + String(sdSettings.checkIntervalHours) + "\n";

  return out;
}
