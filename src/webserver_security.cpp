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
#include "build_version.h" // FIRMWARE_VERSION

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
          "later) can be restored - older exports have nothing for it to read.</p>";
  html += "<form method=\"POST\" action=\"/import\" onsubmit=\"return confirm('Import this "
          "configuration? This REPLACES cameras/Telegram users/network/SD settings currently "
          "stored with whatever the file contains (a section missing from the file is left "
          "alone). Passwords will need to be re-entered - if the file includes Network, do NOT "
          "reboot until you have re-entered the WiFi password, or the board will be unable to "
          "reconnect.');\">";
  html += "<label>Configuration file (.txt from Export above)"
          "<input type=\"file\" accept=\".txt\" onchange=\""
          "var f=this.files[0];if(!f)return;"
          "var r=new FileReader();"
          "r.onload=function(){document.getElementById('importText').value=r.result;};"
          "r.readAsText(f);\"></label>";
  html += "<textarea id=\"importText\" name=\"configText\" style=\"display:none\"></textarea>";
  html += "<p><button type=\"submit\">Import configuration</button></p></form></fieldset>";
  return html;
}

ConfigImportApplyResult applyConfigImport(const String& text) {
  ConfigImportResult parsed = parseConfigImport(text);
  ConfigImportApplyResult result;

  if (parsed.camerasFound) {
    result.anyDomainFound = true;
    if (replaceAllCameras(parsed.cameras)) {
      result.camerasImported = true;
      result.cameraCount = parsed.cameras.size();
    }
  }
  if (parsed.usersFound) {
    result.anyDomainFound = true;
    if (replaceAllTelegramUsers(parsed.users)) {
      result.usersImported = true;
      result.userCount = parsed.users.size();
    }
  }
  if (parsed.networkFound) {
    result.anyDomainFound = true;
    result.networkImported = saveWifiCredentials(parsed.network);
  }
  if (parsed.sdSettingsFound) {
    result.anyDomainFound = true;
    result.sdSettingsImported = saveSdSettings(parsed.sdSettings);
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
