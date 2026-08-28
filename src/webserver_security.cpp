#include "webserver_security.h"
#include "auth_store.h"
#include "format_utils.h"
#include "camera_store.h"
#include "telegram_users.h"
#include "network_store.h"
#include "sd_store.h"

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
  return html;
}

String buildConfigExport() {
  std::vector<CameraConfig> cams = loadCameras();
  std::vector<TelegramUser> users = loadTelegramUsers();
  WifiCredentials net = loadWifiCredentials();

  String out;
  out += "=== Camera Monitor Configuration Export ===\n";
  out += "Firmware build: " + String(__DATE__) + " " + String(__TIME__) + "\n";
  out += "Board uptime at export: " + formatUptime(millis()) + "\n\n";
  out += "Passwords (camera and WiFi) are deliberately NOT included below - re-enter them\n";
  out += "manually after a restore. Everything else here is what's tedious to reconstruct\n";
  out += "from memory via the dashboard.\n";

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
           (c.snapshotUriOverride.length() > 0 ? c.snapshotUriOverride : String("(none)")) + "\n";
    out += "    Preferred profile keyword: " +
           (c.preferredProfileKeyword.length() > 0 ? c.preferredProfileKeyword : String("(none)")) + "\n";
    out += "    Alert cooldown: " + String(c.alertCooldownMs / 1000UL) + "s\n";
    out += "    Offline threshold: " + String(c.offlineThresholdMs / 60000UL) + "min\n";
    out += "    Snapshot burst count: " + String(c.snapshotBurstCount) + "\n";
    out += "    Notes: " + (c.notes.length() > 0 ? c.notes : String("(none)")) + "\n";
  }

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

  out += "\n--- Storage ---\n";
  out += "SD card storage: " + String(loadSdSettings().enabled ? "enabled" : "disabled") + "\n";

  return out;
}
