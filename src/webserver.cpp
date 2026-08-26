#include "webserver.h"
#include "telegram_users.h"
#include "network_store.h"
#include "auth_store.h"
#include <PsychicHttp.h>
#include <WiFi.h>
#include <cctype>
#include <cstdio>
#include <Update.h>
#include <esp_ota_ops.h>

static PsychicHttpServer server;
static std::vector<CameraConfig>* g_liveCameras = nullptr;
static std::vector<CameraState>*  g_liveStates  = nullptr;

// Global middleware, applied to every request in startWebServer() below -
// AuthenticationMiddleware::run() only actually requires a login once both
// setUsername() and setPassword() have been given non-empty values (see
// PsychicHttp's PsychicMiddlewares.cpp), so leaving it unconfigured is what
// makes the board boot with no login required. renderSecurityPanel's save
// handler updates it live via setUsername()/setPassword() the moment a
// password is set or changed, so that takes effect on the very next
// request - no reboot needed.
static AuthenticationMiddleware g_authMiddleware;

static String formatUptime(unsigned long ms) {
  unsigned long totalSec = ms / 1000UL;
  unsigned long days  = totalSec / 86400UL;
  unsigned long hours = (totalSec % 86400UL) / 3600UL;
  unsigned long mins  = (totalSec % 3600UL) / 60UL;
  String s;
  if (days > 0) s += String(days) + "d ";
  s += String(hours) + "h " + String(mins) + "m";
  return s;
}

static String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      default:   out += c;        break;
    }
  }
  return out;
}

// Percent-encodes anything outside the URL-safe set - used for putting a
// camera name (mostly unrestricted free text) into a query string, e.g.
// /cameras/edit?name=...
static String urlEncode(const String& s) {
  String out;
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// "Xh Ym ago" (reusing formatUptime's formatting) for a millis() timestamp,
// or "just now" for anything under a minute - formatUptime alone would
// print "0h 0m ago" for a fresh alert, which reads like stale data.
static String formatElapsedSince(unsigned long ms) {
  unsigned long elapsed = millis() - ms;
  if (elapsed < 60000UL) return "just now";
  return formatUptime(elapsed) + " ago";
}

// ============================================================
// Dashboard shell - sidebar (Network / Cameras / Telegram Users) + content
// panel. All three sections are separate server-rendered pages under plain
// links (no client-side router/JS framework) - this project embeds
// everything in the firmware binary rather than serving from a filesystem,
// so the goal is a dashboard LOOK without the extra moving parts (LittleFS,
// a JS router, fetch-based page loading) that a bigger multi-page app would
// justify.
// ============================================================

enum class Tab { None, Network, Cameras, Users, Firmware, Security };

static String renderShell(Tab active, const String& banner, const String& contentHtml) {
  String html;
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<title>Camera Monitor</title><style>";
  html += "*{box-sizing:border-box;}";
  html += "body{font-family:sans-serif;margin:0;display:flex;min-height:100vh;color:#222;}";
  html += ".sidebar{width:200px;flex-shrink:0;background:#1f2937;color:#e5e7eb;padding:20px 0;}";
  html += ".sidebar .brand{font-weight:bold;font-size:16px;padding:0 20px 20px;}";
  html += ".sidebar a{display:block;padding:10px 20px;color:#cbd5e1;text-decoration:none;font-size:14px;}";
  html += ".sidebar a:hover{background:#374151;}";
  html += ".sidebar a.active{background:#2563eb;color:#fff;font-weight:bold;}";
  html += ".content{flex:1;padding:24px 28px;max-width:960px;}";
  html += "h1{font-size:20px;margin-top:0;}";
  html += "table{border-collapse:collapse;width:100%;margin-bottom:24px;}";
  html += "th,td{border:1px solid #ccc;padding:6px 8px;text-align:left;font-size:14px;vertical-align:top;}";
  html += "th{background:#f0f0f0;}";
  html += "form.inline{display:inline;}";
  html += "fieldset{margin-bottom:20px;}";
  html += "label{display:block;margin-top:10px;font-size:14px;}";
  html += "label.checkbox{display:flex;align-items:center;gap:6px;font-weight:normal;}";
  html += "label.checkbox input{width:auto;}";
  html += "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:5px;margin-top:2px;}";
  html += ".camera-list{border:1px solid #ddd;padding:8px;max-height:180px;overflow-y:auto;margin-top:2px;}";
  html += ".camera-list label{margin-top:2px;}";
  html += ".banner{background:#fffae0;border:1px solid #e0d080;padding:8px 12px;margin-bottom:16px;}";
  html += ".banner-warn{background:#fde2e1;border:1px solid #e08080;padding:8px 12px;margin-bottom:16px;}";
  html += ".hint{color:#666;font-size:13px;}";
  html += "</style></head><body>";

  html += "<nav class=\"sidebar\"><div class=\"brand\">Camera Monitor</div>";
  html += "<a href=\"/network\" class=\"";
  html += (active == Tab::Network) ? "active" : "";
  html += "\">Network</a>";
  html += "<a href=\"/cameras\" class=\"";
  html += (active == Tab::Cameras) ? "active" : "";
  html += "\">Cameras</a>";
  html += "<a href=\"/users\" class=\"";
  html += (active == Tab::Users) ? "active" : "";
  html += "\">Telegram Users</a>";
  html += "<a href=\"/firmware\" class=\"";
  html += (active == Tab::Firmware) ? "active" : "";
  html += "\">Firmware</a>";
  html += "<a href=\"/security\" class=\"";
  html += (active == Tab::Security) ? "active" : "";
  html += "\">Security</a>";
  html += "</nav>";

  html += "<main class=\"content\">";
  DashboardAuth currentAuth = loadDashboardAuth();
  if (currentAuth.username.length() == 0 || currentAuth.password.length() == 0) {
    html += "<div class=\"banner-warn\">No dashboard password is set - anyone on your LAN can view "
            "and change everything here, including WiFi/camera credentials and the Firmware page. "
            "<a href=\"/security\">Set one now</a>.</div>";
  }
  if (banner.length() > 0) html += "<div class=\"banner\">" + banner + "</div>";
  html += contentHtml;
  html += "</main></body></html>";
  return html;
}

// Finds cfg's matching live (currently-running) index by name, or -1 if
// this camera was added since the last reboot and isn't running yet, or was
// deleted and is still running until the next reboot.
static int findLiveCameraIndex(const String& name) {
  if (!g_liveCameras) return -1;
  for (size_t i = 0; i < g_liveCameras->size(); i++) {
    if ((*g_liveCameras)[i].name.equalsIgnoreCase(name)) return (int)i;
  }
  return -1;
}

// ============================================================
// Network panel
// ============================================================

static String renderNetworkPanel() {
  WifiCredentials creds = loadWifiCredentials();

  String connectedRole;
  if (WiFi.SSID() == creds.primary.ssid) connectedRole = " (primary)";
  else if (creds.backup.ssid.length() > 0 && WiFi.SSID() == creds.backup.ssid) connectedRole = " (backup)";

  String html = "<h1>Network</h1>";
  html += "<table>";
  html += "<tr><th>Connected SSID</th><td>" + htmlEscape(WiFi.SSID()) + connectedRole + "</td></tr>";
  html += "<tr><th>IP address</th><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><th>MAC address</th><td>" + WiFi.macAddress() + "</td></tr>";
  html += "<tr><th>Signal (RSSI)</th><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  html += "<tr><th>mDNS address</th><td>http://" + htmlEscape(creds.hostname) + ".local/</td></tr>";
  html += "<tr><th>Uptime</th><td>" + formatUptime(millis()) + "</td></tr>";
  html += "</table>";

  html += "<fieldset><legend>Primary WiFi</legend><form method=\"POST\" action=\"/network/save\">";
  html += "<label>SSID<input type=\"text\" name=\"ssid\" value=\"" + htmlEscape(creds.primary.ssid) + "\" required></label>";
  html += "<label>Password (leave blank to keep the current password)"
          "<input type=\"password\" name=\"password\" placeholder=\"(unchanged)\"></label>";

  html += "<label style=\"margin-top:20px;\">Backup SSID (optional - tried if primary doesn't connect "
          "within 30s; leave blank to disable)<input type=\"text\" name=\"backupSsid\" value=\"" +
          htmlEscape(creds.backup.ssid) + "\"></label>";
  html += "<label>Backup password (leave blank to keep the current one, if backup SSID is unchanged)"
          "<input type=\"password\" name=\"backupPassword\" placeholder=\"(unchanged)\"></label>";
  html += "<p class=\"hint\">If the backup connects when primary doesn't, it's promoted to primary "
          "(and primary demoted to backup) automatically, so future boots try whichever network "
          "actually works first.</p>";

  html += "<label style=\"margin-top:20px;\">Hostname - letters, digits, hyphens only, no spaces or dots "
          "(reachable at http://&lt;hostname&gt;.local/)"
          "<input type=\"text\" name=\"hostname\" value=\"" + htmlEscape(creds.hostname) + "\" required></label>";

  // Applies to whichever of primary/backup ends up connecting - see
  // network_store.h's comment on WifiCredentials::useStaticIP.
  html += "<div style=\"margin-top:20px;\"><label class=\"checkbox\">"
          "<input type=\"radio\" name=\"ipMode\" value=\"dhcp\" id=\"ipModeDhcp\"" +
          String(creds.useStaticIP ? "" : " checked") + "> DHCP</label>";
  html += "<label class=\"checkbox\"><input type=\"radio\" name=\"ipMode\" value=\"static\" id=\"ipModeStatic\"" +
          String(creds.useStaticIP ? " checked" : "") + "> Static IP</label></div>";
  html += "<label>IP address<input type=\"text\" name=\"staticIP\" id=\"staticIP\"></label>";
  html += "<label>Subnet mask<input type=\"text\" name=\"staticSubnet\" id=\"staticSubnet\"></label>";
  html += "<label>Gateway<input type=\"text\" name=\"staticGateway\" id=\"staticGateway\"></label>";
  html += "<label>DNS server (optional - falls back to the gateway if left blank)"
          "<input type=\"text\" name=\"staticDNS\" id=\"staticDNS\"></label>";

  // DHCP mode shows the board's current live-obtained settings (grayed
  // out, informational only); Static mode shows the stored static values
  // (editable). Disabled inputs don't get submitted with the form, so
  // switching to DHCP and saving correctly omits the static fields -
  // handleSaveNetwork keeps whatever static values were already stored.
  html += "<script>";
  html += "var netLive={ip:'" + WiFi.localIP().toString() + "',subnet:'" + WiFi.subnetMask().toString() +
          "',gateway:'" + WiFi.gatewayIP().toString() + "',dns:'" + WiFi.dnsIP().toString() + "'};";
  html += "var netStatic={ip:'" + creds.staticIP + "',subnet:'" + creds.staticSubnet +
          "',gateway:'" + creds.staticGateway + "',dns:'" + creds.staticDNS + "'};";
  html += "var netFieldIds={ip:'staticIP',subnet:'staticSubnet',gateway:'staticGateway',dns:'staticDNS'};"
          "function applyIpMode(){"
          "var isStatic=document.getElementById('ipModeStatic').checked;"
          "var vals=isStatic?netStatic:netLive;"
          "for(var k in netFieldIds){"
          "var el=document.getElementById(netFieldIds[k]);"
          "el.disabled=!isStatic;el.value=vals[k];}}"
          "document.getElementById('ipModeDhcp').addEventListener('change',applyIpMode);"
          "document.getElementById('ipModeStatic').addEventListener('change',applyIpMode);"
          "applyIpMode();";
  html += "</script>";

  html += "<label style=\"margin-top:20px;\">NTP server (always uses UDP port 123 - not configurable "
          "on this platform)<input type=\"text\" name=\"ntpServer\" value=\"" +
          htmlEscape(creds.ntpServer) + "\" required></label>";
  html += "<label>Resync interval, minutes<input type=\"text\" name=\"ntpSyncMinutes\" value=\"" +
          String(creds.ntpSyncIntervalMs / 60000UL) + "\"></label>";

  html += "<p><button type=\"submit\">Save</button></p></form></fieldset>";

  html += "<p class=\"hint\">Saving updates storage immediately, but only takes effect after "
          "the board reboots - a live change could drop it off the network with no way back to "
          "this page if the new credentials are wrong.</p>";
  return html;
}

// mDNS hostnames only support letters, digits, and hyphens - strip anything
// else rather than rejecting the whole save, so a stray pasted space or dot
// doesn't produce a hostname that silently fails to resolve.
static String sanitizeHostname(const String& raw) {
  String out;
  out.reserve(raw.length());
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (isalnum((unsigned char)c) || c == '-') out += c;
  }
  return out;
}

static void handleSaveNetwork(PsychicRequest* request, String& banner) {
  WifiCredentials creds = loadWifiCredentials(); // current values, so a blank field keeps them

  String ssid = request->getParam("ssid", "");
  ssid.trim();
  if (ssid.length() > 0) creds.primary.ssid = ssid;

  String password = request->getParam("password", "");
  if (password.length() > 0) creds.primary.password = password;

  // Backup SSID isn't "blank keeps current" like the passwords - clearing
  // it is the only way to disable a configured backup, so an empty
  // submission directly clears both backup fields instead of preserving
  // whatever was there before.
  String backupSsid = request->getParam("backupSsid", "");
  backupSsid.trim();
  if (backupSsid.length() == 0) {
    creds.backup.ssid = "";
    creds.backup.password = "";
  } else {
    creds.backup.ssid = backupSsid;
    String backupPassword = request->getParam("backupPassword", "");
    if (backupPassword.length() > 0) creds.backup.password = backupPassword;
  }

  String hostname = sanitizeHostname(request->getParam("hostname", ""));
  if (hostname.length() > 0) creds.hostname = hostname;

  if (creds.primary.ssid.length() == 0) {
    banner = "Primary SSID is required - not saved.";
    return;
  }

  // Static fields are disabled (and so never submitted) when DHCP is
  // selected - keep whatever was already stored so switching back to
  // Static later doesn't lose it. Only touch/validate them when the form
  // actually submitted ipMode=static.
  String ipMode = request->getParam("ipMode", "dhcp");
  creds.useStaticIP = (ipMode == "static");
  if (creds.useStaticIP) {
    String ip = request->getParam("staticIP", "");
    String subnet = request->getParam("staticSubnet", "");
    String gateway = request->getParam("staticGateway", "");
    String dns = request->getParam("staticDNS", "");
    ip.trim(); subnet.trim(); gateway.trim(); dns.trim();

    IPAddress parsed;
    if (ip.length() == 0 || subnet.length() == 0 || gateway.length() == 0 ||
        !parsed.fromString(ip) || !parsed.fromString(subnet) || !parsed.fromString(gateway) ||
        (dns.length() > 0 && !parsed.fromString(dns))) {
      banner = "Static IP, subnet mask, and gateway must be valid addresses (DNS is optional) - not saved.";
      return;
    }
    creds.staticIP = ip;
    creds.staticSubnet = subnet;
    creds.staticGateway = gateway;
    creds.staticDNS = dns;
  }

  String ntpServer = request->getParam("ntpServer", "");
  ntpServer.trim();
  if (ntpServer.length() > 0) creds.ntpServer = ntpServer;

  long ntpMinutes = request->getParam("ntpSyncMinutes", "60").toInt();
  if (ntpMinutes > 0) creds.ntpSyncIntervalMs = (unsigned long)ntpMinutes * 60000UL;
  // else keep whatever was already stored - a blank/zero/negative field
  // shouldn't produce a 0ms (hammer-the-server) resync interval.

  saveWifiCredentials(creds);
  banner = "Saved - reboot the board to apply the new network configuration.";
}

// ============================================================
// Cameras panel
// ============================================================

// Shared by the "Add camera" form (v = a fresh, default-valued CameraConfig),
// "Edit camera" (v = the stored record, password blanked by the caller), and
// a post-Test-Connection redisplay (v = whatever was just submitted, so a
// test doesn't throw away what you typed). isEdit controls the legend/button
// text and whether a hidden originalName field is emitted - see
// saveCameraSubmission for what that's used for.
static String renderCameraForm(const CameraConfig& v, bool isEdit) {
  String html;
  String legend = isEdit ? ("Edit camera: " + htmlEscape(v.name)) : "Add camera";
  html += "<fieldset><legend>" + legend + "</legend><form method=\"POST\" action=\"/cameras/save\">";
  if (isEdit) {
    html += "<input type=\"hidden\" name=\"originalName\" value=\"" + htmlEscape(v.name) + "\">";
  }
  html += "<label>Name (unique)<input type=\"text\" name=\"name\" value=\"" + htmlEscape(v.name) +
          "\" required></label>";
  html += "<label>Device service URL, e.g. http://192.168.1.50/onvif/device_service"
          "<input type=\"text\" name=\"deviceServiceUrl\" value=\"" + htmlEscape(v.deviceServiceUrl) +
          "\" required></label>";
  html += "<label>Username<input type=\"text\" name=\"user\" value=\"" + htmlEscape(v.user) + "\"></label>";
  html += "<label>Password" + String(isEdit ? " (leave blank to keep the current password)" : "") +
          "<input type=\"password\" name=\"pass\"" +
          String(isEdit ? " placeholder=\"(unchanged)\"" : "") + "></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
          String(v.enabled ? " checked" : "") + "> Enabled</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"useWSSecurity\"" +
          String(v.useWSSecurity ? " checked" : "") +
          "> Use WS-Security (uncheck for HTTP Basic Auth)</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"includeInitialTerminationTime\"" +
          String(v.includeInitialTerminationTime ? " checked" : "") + "> Include InitialTerminationTime</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"includeReplyToAnonymous\"" +
          String(v.includeReplyToAnonymous ? " checked" : "") + "> Include ReplyTo anonymous</label>";
  html += "<label>Snapshot URI override (optional; {USER}/{PASS} substituted at runtime)"
          "<input type=\"text\" name=\"snapshotUriOverride\" value=\"" +
          htmlEscape(v.snapshotUriOverride) + "\"></label>";
  html += "<label>Preferred profile keyword (optional, e.g. \"sub\")"
          "<input type=\"text\" name=\"preferredProfileKeyword\" value=\"" +
          htmlEscape(v.preferredProfileKeyword) + "\"></label>";
  html += "<label>Alert cooldown, seconds (minimum time between Telegram alerts for this camera)"
          "<input type=\"text\" name=\"alertCooldownSec\" value=\"" + String(v.alertCooldownMs / 1000) +
          "\"></label>";
  html += "<label>Offline threshold, minutes (no response for this long -> OFFLINE alert)"
          "<input type=\"text\" name=\"offlineThresholdMin\" value=\"" + String(v.offlineThresholdMs / 60000UL) +
          "\"></label>";
  html += "<label>Notes<input type=\"text\" name=\"notes\" value=\"" + htmlEscape(v.notes) + "\"></label>";
  html += "<p><button type=\"submit\" formaction=\"/cameras/save\">" +
          String(isEdit ? "Save changes" : "Add camera") + "</button> ";
  html += "<button type=\"submit\" formaction=\"/cameras/test\">Test Connection</button>";
  if (isEdit) html += " <a href=\"/cameras\">Cancel</a>";
  html += "</p></form></fieldset>";
  return html;
}

// prefill/isEdit repopulate the Add/Edit form after an edit-link click, a
// failed save, or a Test Connection round trip - null prefill is the normal
// blank "Add camera" state.
static String renderCamerasPanel(const CameraConfig* prefill, bool isEdit) {
  std::vector<CameraConfig> cams = loadCameras();

  String html = "<h1>Cameras</h1>";
  html += "<table><tr><th>Name</th><th>Device Service URL</th><th>Enabled</th>"
          "<th>Cooldown</th><th>Offline After</th><th>Live Status</th><th>Last Alert</th>"
          "<th>Notes</th><th></th></tr>";
  for (auto& c : cams) {
    int idx = findLiveCameraIndex(c.name);
    String liveStatus;
    String lastAlertStr = "never";
    if (idx >= 0 && g_liveStates && idx < (int)g_liveStates->size()) {
      CameraState& st = (*g_liveStates)[idx];
      liveStatus = st.subscriptionActive ? "subscribed" : "not subscribed";
      if (st.isOffline) liveStatus += " - OFFLINE";
      if (!st.alertsEnabled) liveStatus += " (alerts OFF)";
      if (st.hasAlerted) lastAlertStr = formatElapsedSince(st.lastAlert);
    } else if (!c.enabled) {
      liveStatus = "disabled";
    } else {
      liveStatus = "not running - reboot to apply";
    }

    html += "<tr><td>" + htmlEscape(c.name) + "</td><td>" + htmlEscape(c.deviceServiceUrl) +
            "</td><td>" + (c.enabled ? "yes" : "no") + "</td><td>" +
            String(c.alertCooldownMs / 1000) + "s</td><td>" +
            String(c.offlineThresholdMs / 60000UL) + "m</td><td>" + liveStatus + "</td><td>" +
            lastAlertStr + "</td><td>" +
            htmlEscape(c.notes) + "</td><td>";
    html += "<a href=\"/cameras/edit?name=" + urlEncode(c.name) + "\">Edit</a> ";
    html += "<form class=\"inline\" method=\"POST\" action=\"/delete\" "
            "onsubmit=\"return confirm('Delete " + htmlEscape(c.name) + "?');\">";
    html += "<input type=\"hidden\" name=\"name\" value=\"" + htmlEscape(c.name) + "\">";
    html += "<button type=\"submit\">Delete</button></form></td></tr>";
  }
  html += "</table>";

  html += renderCameraForm(prefill ? *prefill : CameraConfig(), isEdit);

  html += "<p class=\"hint\">Adding, editing, or deleting a camera updates storage immediately, "
          "but only takes effect after the board reboots. Test Connection doesn't save anything - "
          "it just runs GetCapabilities/GetEventProperties/GetSnapshotUri against whatever is "
          "currently typed in, so you can catch a wrong URL or credential before rebooting.</p>";
  return html;
}

// Reads the Add/Edit camera form's fields into a CameraConfig - used both to
// actually save (saveCameraSubmission) and to test a connection without
// saving (testCameraConnection).
static CameraConfig parseCameraForm(PsychicRequest* request) {
  CameraConfig c;
  c.name                          = request->getParam("name", "");
  c.deviceServiceUrl              = request->getParam("deviceServiceUrl", "");
  c.enabled                       = request->hasParam("enabled");
  c.useWSSecurity                 = request->hasParam("useWSSecurity");
  c.includeInitialTerminationTime = request->hasParam("includeInitialTerminationTime");
  c.includeReplyToAnonymous       = request->hasParam("includeReplyToAnonymous");
  c.snapshotUriOverride           = request->getParam("snapshotUriOverride", "");
  c.preferredProfileKeyword       = request->getParam("preferredProfileKeyword", "");
  c.user                          = request->getParam("user", "");
  c.pass                          = request->getParam("pass", "");
  c.notes                         = request->getParam("notes", "");
  c.name.trim();

  long cooldownSec = request->getParam("alertCooldownSec", "30").toInt();
  // A blank/zero/negative field shouldn't produce a 0ms cooldown (alerts on
  // every single poll) - fall back to CameraConfig's own default instead.
  c.alertCooldownMs = cooldownSec > 0 ? (unsigned long)cooldownSec * 1000UL : CameraConfig().alertCooldownMs;

  long offlineMin = request->getParam("offlineThresholdMin", "5").toInt();
  c.offlineThresholdMs = offlineMin > 0 ? (unsigned long)offlineMin * 60000UL : CameraConfig().offlineThresholdMs;

  return c;
}

// originalName is "" for a brand-new camera (add), non-empty for an edit
// (the name the camera had before this submission - cam.name may differ,
// which is a rename). A blank password on an edit means "keep the current
// one", same convention as the Network panel's WiFi password field.
static bool saveCameraSubmission(CameraConfig cam, const String& originalName, String& banner) {
  if (cam.name.length() == 0 || cam.deviceServiceUrl.length() == 0) {
    banner = "Name and device service URL are required - camera not saved.";
    return false;
  }

  if (originalName.length() == 0) {
    if (!addCamera(cam)) {
      banner = "A camera named \"" + htmlEscape(cam.name) + "\" already exists - camera not added.";
      return false;
    }
    return true;
  }

  if (cam.pass.length() == 0) {
    for (auto& existing : loadCameras()) {
      if (existing.name.equalsIgnoreCase(originalName)) { cam.pass = existing.pass; break; }
    }
  }
  if (!updateCamera(originalName, cam)) {
    banner = "Could not save \"" + htmlEscape(cam.name) +
             "\" - a different camera already uses that name.";
    return false;
  }
  return true;
}

// Runs a live GetCapabilities -> GetServiceCapabilities/GetEventProperties
// -> GetProfiles/GetSnapshotUri sequence against cfg without touching NVS or
// any running camera task, and summarizes what worked. Reuses the exact
// same calls cameraSetupSequence (camera.cpp) makes at boot, so a pass here
// is a strong signal the real thing will work too.
static String testCameraConnection(CameraConfig cfg) {
  if (cfg.deviceServiceUrl.length() == 0) {
    return "Enter a device service URL first, then Test Connection.";
  }

  CameraState st;
  if (!resolveCameraCredentials(cfg, st)) {
    return "Enter a username and password first, then Test Connection.";
  }

  if (!cameraDiscoverServices(cfg, st)) {
    return "Test FAILED for \"" + cfg.name + "\": could not reach " + cfg.deviceServiceUrl +
           ", or no ONVIF event service was found there. Check the URL/credentials and see the "
           "Serial log for details.";
  }

  String result = "Test result for \"" + cfg.name + "\": device service reachable, event service found.";

  if (cameraGetEventServiceCapabilities(cfg, st) && cameraGetEventProperties(cfg, st)) {
    result += " Event service responds normally.";
  } else {
    result += " WARNING: the event service didn't respond to GetServiceCapabilities/GetEventProperties - "
              "this camera may not support ONVIF eventing at all.";
  }

  if (cfg.snapshotUriOverride.length() > 0 || st.mediaServiceUrl.length() > 0) {
    if (cameraFetchProfileAndSnapshotUri(cfg, st) && st.snapshotUri.length() > 0) {
      result += " Snapshot URI resolved.";
    } else {
      result += " WARNING: snapshot URI could not be resolved - motion would still be detected, "
                "but photo alerts won't work until this is fixed.";
    }
  } else {
    result += " WARNING: no media service found and no snapshot override set - photo alerts won't work.";
  }

  return result;
}

// ============================================================
// Telegram Users panel
// ============================================================

// Shared by the "Add Telegram user" form (v = a fresh TelegramUser with
// allCameras forced true, matching the old hardcoded default), "Edit user"
// (v = the stored record), and a failed-save redisplay (v = whatever was
// just submitted). isEdit controls the legend/button text and whether a
// hidden originalName field is emitted - see saveUserSubmission for what
// that's used for.
static String renderTelegramUserForm(const TelegramUser& v, const std::vector<CameraConfig>& cams, bool isEdit) {
  String html;
  String legend = isEdit ? ("Edit Telegram user: " + htmlEscape(v.name)) : "Add Telegram user";
  html += "<fieldset><legend>" + legend + "</legend><form method=\"POST\" action=\"/users/save\">";
  if (isEdit) {
    html += "<input type=\"hidden\" name=\"originalName\" value=\"" + htmlEscape(v.name) + "\">";
  }
  html += "<label>Name (unique)<input type=\"text\" name=\"name\" value=\"" + htmlEscape(v.name) +
          "\" required></label>";
  html += "<label>Chat ID (message @userinfobot, or check "
          "https://api.telegram.org/bot&lt;TOKEN&gt;/getUpdates after messaging your bot)"
          "<input type=\"text\" name=\"chatId\" value=\"" + htmlEscape(v.chatId) + "\" required></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"allCameras\"" +
          String(v.allCameras ? " checked" : "") + "> All cameras (including ones added later)</label>";
  html += "<label>Or pick specific cameras (ignored if \"All cameras\" is checked):</label>";
  html += "<div class=\"camera-list\">";
  if (cams.empty()) {
    html += "<span class=\"hint\">No cameras defined yet.</span>";
  }
  for (auto& c : cams) {
    bool checked = false;
    for (auto& n : v.cameraNames) {
      if (n.equalsIgnoreCase(c.name)) { checked = true; break; }
    }
    html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"cam_" + htmlEscape(c.name) + "\"" +
            String(checked ? " checked" : "") + "> " + htmlEscape(c.name) + "</label>";
  }
  html += "</div>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"systemMessages\"" +
          String(v.systemMessages ? " checked" : "") + "> Receive heartbeat and boot-online messages</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"canCommand\"" +
          String(v.canCommand ? " checked" : "") + "> May send /on, /off, /status commands</label>";
  html += "<p><button type=\"submit\">" + String(isEdit ? "Save changes" : "Add user") + "</button>";
  if (isEdit) html += " <a href=\"/users\">Cancel</a>";
  html += "</p></form></fieldset>";
  return html;
}

// prefill/isEdit repopulate the Add/Edit form after an edit-link click or a
// failed save - null prefill is the normal blank "Add Telegram user" state.
static String renderUsersPanel(const TelegramUser* prefill, bool isEdit) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  std::vector<CameraConfig> cams = loadCameras();

  String html = "<h1>Telegram Users</h1>";
  html += "<table><tr><th>Name</th><th>Chat ID</th><th>Cameras</th>"
          "<th>System Messages</th><th>Can Command</th><th></th></tr>";
  for (auto& u : users) {
    String camerasCol;
    if (u.allCameras) {
      camerasCol = "All";
    } else if (u.cameraNames.empty()) {
      camerasCol = "(none)";
    } else {
      for (size_t i = 0; i < u.cameraNames.size(); i++) {
        if (i > 0) camerasCol += ", ";
        camerasCol += htmlEscape(u.cameraNames[i]);
      }
    }

    html += "<tr><td>" + htmlEscape(u.name) + "</td><td>" + htmlEscape(u.chatId) + "</td><td>" +
            camerasCol + "</td><td>" + (u.systemMessages ? "yes" : "no") + "</td><td>" +
            (u.canCommand ? "yes" : "no") + "</td><td>";
    html += "<a href=\"/users/edit?name=" + urlEncode(u.name) + "\">Edit</a> ";
    html += "<form class=\"inline\" method=\"POST\" action=\"/users/delete\" "
            "onsubmit=\"return confirm('Delete " + htmlEscape(u.name) + "?');\">";
    html += "<input type=\"hidden\" name=\"name\" value=\"" + htmlEscape(u.name) + "\">";
    html += "<button type=\"submit\">Delete</button></form></td></tr>";
  }
  html += "</table>";

  TelegramUser blankAdd;
  blankAdd.allCameras = true; // friendlier default for a brand-new user than the struct's own false
  html += renderTelegramUserForm(prefill ? *prefill : blankAdd, cams, isEdit);

  html += "<p class=\"hint\">Adding, editing, or deleting a Telegram user takes effect on the next "
          "Telegram poll/alert - no reboot needed (unlike camera changes).</p>";
  return html;
}

// PsychicRequest's public API doesn't expose "all values for a repeated
// param name", so each camera gets its own uniquely-named checkbox
// ("cam_<name>") instead of sharing name="camera" - probe for each known
// camera by name rather than trying to enumerate submitted fields.
static TelegramUser parseUserForm(PsychicRequest* request) {
  TelegramUser u;
  u.name           = request->getParam("name", "");
  u.chatId         = request->getParam("chatId", "");
  u.allCameras     = request->hasParam("allCameras");
  u.systemMessages = request->hasParam("systemMessages");
  u.canCommand     = request->hasParam("canCommand");
  u.name.trim();
  u.chatId.trim();

  if (!u.allCameras) {
    for (auto& c : loadCameras()) {
      if (request->hasParam(("cam_" + c.name).c_str())) {
        u.cameraNames.push_back(c.name);
      }
    }
  }
  return u;
}

// originalName is "" for a brand-new user (add), non-empty for an edit (the
// name the user had before this submission - user.name may differ, which
// is a rename).
static bool saveUserSubmission(const TelegramUser& user, const String& originalName, String& banner) {
  if (user.name.length() == 0 || user.chatId.length() == 0) {
    banner = "Name and Chat ID are required - user not saved.";
    return false;
  }

  if (originalName.length() == 0) {
    if (!addTelegramUser(user)) {
      banner = "A Telegram user named \"" + htmlEscape(user.name) + "\" already exists - user not added.";
      return false;
    }
    return true;
  }

  if (!updateTelegramUser(originalName, user)) {
    banner = "Could not save \"" + htmlEscape(user.name) + "\" - a different user already uses that name.";
    return false;
  }
  return true;
}

// ============================================================
// Firmware panel - upload a .bin over the dashboard instead of a USB
// reflash. Backed by ESP32's Update library, which writes into the
// currently-inactive OTA app partition (this board's partition table has
// two, app0/app1 - see platformio.ini) and only marks it bootable once the
// image is fully received and its checksum verifies, so a failed/aborted
// upload leaves the running firmware untouched.
// ============================================================

static bool   g_otaError = false;
static String g_otaErrorMsg;

// esp_restart() from inside the request handler that's still sending the
// response would tear down the connection before the client sees it -
// reboot from a short-lived separate task instead, after send() has
// returned, so the browser gets a chance to show the result first.
static void otaRebootTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP.restart();
}

static String renderFirmwarePanel() {
  const esp_partition_t* running = esp_ota_get_running_partition();

  String html = "<h1>Firmware</h1>";
  html += "<table>";
  html += "<tr><th>Build</th><td>" + String(__DATE__) + " " + String(__TIME__) + "</td></tr>";
  html += "<tr><th>Running partition</th><td>" + String(running ? running->label : "?") + "</td></tr>";
  html += "<tr><th>Sketch size</th><td>" + String(ESP.getSketchSize() / 1024) + " KB</td></tr>";
  html += "<tr><th>Free space for update</th><td>" + String(ESP.getFreeSketchSpace() / 1024) + " KB</td></tr>";
  html += "</table>";

  html += "<fieldset><legend>Upload new firmware</legend>";
  html += "<form method=\"POST\" action=\"/firmware/update\" enctype=\"multipart/form-data\" "
          "onsubmit=\"return confirm('Flash this firmware and reboot the board? "
          "Do not power it off while this runs.');\">";
  html += "<label>Firmware .bin (e.g. .pio/build/esp32s3/firmware.bin from `pio run -e esp32s3`)"
          "<input type=\"file\" name=\"firmware\" accept=\".bin\" required></label>";
  html += "<p><button type=\"submit\">Upload &amp; Flash</button></p>";
  html += "</form></fieldset>";

  html += "<p class=\"hint\">The board reboots automatically once the upload finishes and the new "
          "image's checksum verifies. If verification fails, nothing is applied and the board keeps "
          "running the current firmware.</p>";
  return html;
}

// ============================================================
// Security panel - dashboard login (HTTP Basic Auth). See auth_store.h and
// g_authMiddleware's comment: empty username/password (the default) means
// no login is required at all, which is how the board boots. Setting one
// here takes effect on the very next request, no reboot needed.
// ============================================================

static String renderSecurityPanel() {
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
  return html;
}

void startWebServer(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  g_liveCameras = liveCameras;
  g_liveStates = liveStates;

  DashboardAuth auth = loadDashboardAuth();
  g_authMiddleware.setUsername(auth.username.c_str())
      .setPassword(auth.password.c_str())
      .setRealm("Camera Monitor")
      .setAuthMethod(BASIC_AUTH);
  // Applies to every route registered below, including the Firmware
  // upload - AuthenticationMiddleware::run() is a no-op until both fields
  // above are non-empty, which is what lets the board boot with no login
  // required (see g_authMiddleware's declaration comment).
  server.addMiddleware(&g_authMiddleware);

  server.on("/", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    String landing = "<h1>Camera Monitor</h1><p class=\"hint\">Select a section from the left.</p>";
    return response->send(200, "text/html", renderShell(Tab::None, "", landing).c_str());
  });

  server.on("/network", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Network, "", renderNetworkPanel()).c_str());
  });

  server.on("/network/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner;
    handleSaveNetwork(request, banner);
    return response->send(200, "text/html", renderShell(Tab::Network, banner, renderNetworkPanel()).c_str());
  });

  server.on("/cameras", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Cameras, "", renderCamerasPanel(nullptr, false)).c_str());
  });

  server.on("/cameras/edit", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    for (auto& c : loadCameras()) {
      if (c.name.equalsIgnoreCase(name)) {
        CameraConfig prefill = c;
        prefill.pass = ""; // never populate a password field with the real value
        return response->send(200, "text/html",
                               renderShell(Tab::Cameras, "", renderCamerasPanel(&prefill, true)).c_str());
      }
    }
    return response->redirect("/cameras");
  });

  server.on("/cameras/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    CameraConfig submitted = parseCameraForm(request);
    String originalName = request->getParam("originalName", "");
    originalName.trim();

    String banner;
    if (!saveCameraSubmission(submitted, originalName, banner)) {
      submitted.pass = "";
      return response->send(200, "text/html",
                             renderShell(Tab::Cameras, banner,
                                         renderCamerasPanel(&submitted, originalName.length() > 0)).c_str());
    }
    return response->redirect("/cameras");
  });

  server.on("/cameras/test", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    CameraConfig submitted = parseCameraForm(request);
    String originalName = request->getParam("originalName", "");
    originalName.trim();
    bool isEdit = originalName.length() > 0;

    CameraConfig testCfg = submitted;
    if (isEdit && testCfg.pass.length() == 0) {
      for (auto& existing : loadCameras()) {
        if (existing.name.equalsIgnoreCase(originalName)) { testCfg.pass = existing.pass; break; }
      }
    }
    String banner = testCameraConnection(testCfg);

    submitted.pass = "";
    return response->send(200, "text/html",
                           renderShell(Tab::Cameras, banner, renderCamerasPanel(&submitted, isEdit)).c_str());
  });

  server.on("/delete", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    deleteCamera(name);
    return response->redirect("/cameras");
  });

  server.on("/users", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Users, "", renderUsersPanel(nullptr, false)).c_str());
  });

  server.on("/users/edit", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    for (auto& u : loadTelegramUsers()) {
      if (u.name.equalsIgnoreCase(name)) {
        TelegramUser prefill = u;
        return response->send(200, "text/html",
                               renderShell(Tab::Users, "", renderUsersPanel(&prefill, true)).c_str());
      }
    }
    return response->redirect("/users");
  });

  server.on("/users/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    TelegramUser submitted = parseUserForm(request);
    String originalName = request->getParam("originalName", "");
    originalName.trim();

    String banner;
    if (!saveUserSubmission(submitted, originalName, banner)) {
      return response->send(200, "text/html",
                             renderShell(Tab::Users, banner,
                                         renderUsersPanel(&submitted, originalName.length() > 0)).c_str());
    }
    return response->redirect("/users");
  });

  server.on("/users/delete", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    deleteTelegramUser(name);
    return response->redirect("/users");
  });

  server.on("/firmware", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Firmware, "", renderFirmwarePanel()).c_str());
  });

  static PsychicUploadHandler* otaHandler = new PsychicUploadHandler();
  otaHandler->onUpload([](PsychicRequest* request, const String& filename, uint64_t index, uint8_t* data,
                           size_t len, bool last) -> esp_err_t {
    if (index == 0) {
      g_otaError = false;
      g_otaErrorMsg = "";
      Serial.printf("[Firmware] Upload started: %s\n", filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        g_otaError = true;
        g_otaErrorMsg = Update.errorString();
        Serial.printf("[Firmware] Update.begin failed: %s\n", g_otaErrorMsg.c_str());
      }
    }
    if (!g_otaError && len > 0 && Update.write(data, len) != len) {
      g_otaError = true;
      g_otaErrorMsg = Update.errorString();
      Serial.printf("[Firmware] Update.write failed: %s\n", g_otaErrorMsg.c_str());
    }
    if (last) {
      if (!g_otaError && !Update.end(true)) {
        g_otaError = true;
        g_otaErrorMsg = Update.errorString();
      }
      Serial.printf("[Firmware] Upload finished (%s).\n", g_otaError ? "FAILED" : "OK - rebooting");
    }
    return ESP_OK; // keep accepting bytes even after a failure, so the upload doesn't just hang client-side
  });
  otaHandler->onRequest([](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    if (g_otaError) {
      String banner = "Firmware update FAILED: " + g_otaErrorMsg + " - current firmware keeps running.";
      return response->send(200, "text/html", renderShell(Tab::Firmware, banner, renderFirmwarePanel()).c_str());
    }
    esp_err_t result = response->send(
        200, "text/html",
        renderShell(Tab::Firmware, "Firmware accepted - rebooting now, this page will stop responding.",
                    "<p class=\"hint\">Reconnect in about 15 seconds.</p>")
            .c_str());
    xTaskCreate(otaRebootTask, "otaReboot", 2048, nullptr, 1, nullptr);
    return result;
  });
  server.on("/firmware/update", HTTP_POST, otaHandler);

  server.on("/security", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Security, "", renderSecurityPanel()).c_str());
  });

  server.on("/security/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String username = request->getParam("username", "");
    String password = request->getParam("password", "");
    String confirmPassword = request->getParam("confirmPassword", "");
    username.trim();

    String banner;
    if (username.length() == 0 || password.length() == 0) {
      banner = "Username and password are both required.";
    } else if (password != confirmPassword) {
      banner = "Password and confirmation don't match - not saved.";
    } else {
      DashboardAuth newAuth;
      newAuth.username = username;
      newAuth.password = password;
      saveDashboardAuth(newAuth);
      g_authMiddleware.setUsername(newAuth.username.c_str()).setPassword(newAuth.password.c_str());
      banner = "Saved - a login is now required on every page, starting now.";
    }
    return response->send(200, "text/html", renderShell(Tab::Security, banner, renderSecurityPanel()).c_str());
  });

  server.begin();
  Serial.println("[WebServer] Camera management UI listening on port 80.");
}
