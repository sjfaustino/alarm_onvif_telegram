#include "webserver.h"
#include "telegram_users.h"
#include "network_store.h"
#include <PsychicHttp.h>
#include <WiFi.h>
#include <cctype>

static PsychicHttpServer server;
static std::vector<CameraConfig>* g_liveCameras = nullptr;
static std::vector<CameraState>*  g_liveStates  = nullptr;

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

// ============================================================
// Dashboard shell - sidebar (Network / Cameras / Telegram Users) + content
// panel. All three sections are separate server-rendered pages under plain
// links (no client-side router/JS framework) - this project embeds
// everything in the firmware binary rather than serving from a filesystem,
// so the goal is a dashboard LOOK without the extra moving parts (LittleFS,
// a JS router, fetch-based page loading) that a bigger multi-page app would
// justify.
// ============================================================

enum class Tab { None, Network, Cameras, Users };

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
  html += "</nav>";

  html += "<main class=\"content\">";
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
  saveWifiCredentials(creds);
  banner = "Saved - reboot the board to apply the new WiFi credentials and/or hostname.";
}

// ============================================================
// Cameras panel
// ============================================================

static String renderCamerasPanel() {
  std::vector<CameraConfig> cams = loadCameras();

  String html = "<h1>Cameras</h1>";
  html += "<table><tr><th>Name</th><th>Device Service URL</th><th>Enabled</th>"
          "<th>Cooldown</th><th>Live Status</th><th>Notes</th><th></th></tr>";
  for (auto& c : cams) {
    int idx = findLiveCameraIndex(c.name);
    String liveStatus;
    if (idx >= 0 && g_liveStates && idx < (int)g_liveStates->size()) {
      CameraState& st = (*g_liveStates)[idx];
      liveStatus = st.subscriptionActive ? "subscribed" : "not subscribed";
      if (st.isOffline) liveStatus += " - OFFLINE";
      if (!st.alertsEnabled) liveStatus += " (alerts OFF)";
    } else if (!c.enabled) {
      liveStatus = "disabled";
    } else {
      liveStatus = "not running - reboot to apply";
    }

    html += "<tr><td>" + htmlEscape(c.name) + "</td><td>" + htmlEscape(c.deviceServiceUrl) +
            "</td><td>" + (c.enabled ? "yes" : "no") + "</td><td>" +
            String(c.alertCooldownMs / 1000) + "s</td><td>" + liveStatus + "</td><td>" +
            htmlEscape(c.notes) + "</td><td>";
    html += "<form class=\"inline\" method=\"POST\" action=\"/delete\" "
            "onsubmit=\"return confirm('Delete " + htmlEscape(c.name) + "?');\">";
    html += "<input type=\"hidden\" name=\"name\" value=\"" + htmlEscape(c.name) + "\">";
    html += "<button type=\"submit\">Delete</button></form></td></tr>";
  }
  html += "</table>";

  html += "<fieldset><legend>Add camera</legend><form method=\"POST\" action=\"/add\">";
  html += "<label>Name (unique)<input type=\"text\" name=\"name\" required></label>";
  html += "<label>Device service URL, e.g. http://192.168.1.50/onvif/device_service"
          "<input type=\"text\" name=\"deviceServiceUrl\" required></label>";
  html += "<label>Username<input type=\"text\" name=\"user\"></label>";
  html += "<label>Password<input type=\"text\" name=\"pass\"></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\" checked> Enabled</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"useWSSecurity\" checked> "
          "Use WS-Security (uncheck for HTTP Basic Auth)</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"includeInitialTerminationTime\"> "
          "Include InitialTerminationTime</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"includeReplyToAnonymous\"> "
          "Include ReplyTo anonymous</label>";
  html += "<label>Snapshot URI override (optional; {USER}/{PASS} substituted at runtime)"
          "<input type=\"text\" name=\"snapshotUriOverride\"></label>";
  html += "<label>Preferred profile keyword (optional, e.g. \"sub\")"
          "<input type=\"text\" name=\"preferredProfileKeyword\"></label>";
  html += "<label>Alert cooldown, seconds (minimum time between Telegram alerts for this camera)"
          "<input type=\"text\" name=\"alertCooldownSec\" value=\"30\"></label>";
  html += "<label>Notes<input type=\"text\" name=\"notes\"></label>";
  html += "<p><button type=\"submit\">Add camera</button></p></form></fieldset>";

  html += "<p class=\"hint\">Adding or deleting a camera updates storage immediately, "
          "but only takes effect after the board reboots.</p>";
  return html;
}

static void handleAddCamera(PsychicRequest* request, String& banner) {
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
  if (cooldownSec > 0) c.alertCooldownMs = (unsigned long)cooldownSec * 1000UL;
  // else keep CameraConfig's 30000 default - a blank/zero/negative field
  // shouldn't produce a 0ms cooldown (alerts on every single poll).

  if (c.name.length() == 0 || c.deviceServiceUrl.length() == 0) {
    banner = "Name and device service URL are required - camera not added.";
    return;
  }
  if (!addCamera(c)) {
    banner = "A camera named \"" + htmlEscape(c.name) + "\" already exists - camera not added.";
  }
}

// ============================================================
// Telegram Users panel
// ============================================================

static String renderUsersPanel() {
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
    html += "<form class=\"inline\" method=\"POST\" action=\"/users/delete\" "
            "onsubmit=\"return confirm('Delete " + htmlEscape(u.name) + "?');\">";
    html += "<input type=\"hidden\" name=\"name\" value=\"" + htmlEscape(u.name) + "\">";
    html += "<button type=\"submit\">Delete</button></form></td></tr>";
  }
  html += "</table>";

  html += "<fieldset><legend>Add Telegram user</legend><form method=\"POST\" action=\"/users/add\">";
  html += "<label>Name (unique)<input type=\"text\" name=\"name\" required></label>";
  html += "<label>Chat ID (message @userinfobot, or check "
          "https://api.telegram.org/bot&lt;TOKEN&gt;/getUpdates after messaging your bot)"
          "<input type=\"text\" name=\"chatId\" required></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"allCameras\" checked> "
          "All cameras (including ones added later)</label>";
  html += "<label>Or pick specific cameras (ignored if \"All cameras\" is checked):</label>";
  html += "<div class=\"camera-list\">";
  if (cams.empty()) {
    html += "<span class=\"hint\">No cameras defined yet.</span>";
  }
  for (auto& c : cams) {
    html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"cam_" + htmlEscape(c.name) +
            "\"> " + htmlEscape(c.name) + "</label>";
  }
  html += "</div>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"systemMessages\"> "
          "Receive heartbeat and boot-online messages</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"canCommand\"> "
          "May send /on, /off, /status commands</label>";
  html += "<p><button type=\"submit\">Add user</button></p></form></fieldset>";

  html += "<p class=\"hint\">Changes take effect on the next Telegram poll/alert - no reboot needed "
          "(unlike camera changes).</p>";
  return html;
}

static void handleAddUser(PsychicRequest* request, String& banner) {
  TelegramUser u;
  u.name           = request->getParam("name", "");
  u.chatId         = request->getParam("chatId", "");
  u.allCameras     = request->hasParam("allCameras");
  u.systemMessages = request->hasParam("systemMessages");
  u.canCommand     = request->hasParam("canCommand");
  u.name.trim();
  u.chatId.trim();

  // PsychicRequest's public API doesn't expose "all values for a repeated
  // param name", so each camera gets its own uniquely-named checkbox
  // ("cam_<name>") instead of sharing name="camera" - probe for each known
  // camera by name rather than trying to enumerate submitted fields.
  if (!u.allCameras) {
    for (auto& c : loadCameras()) {
      if (request->hasParam(("cam_" + c.name).c_str())) {
        u.cameraNames.push_back(c.name);
      }
    }
  }

  if (u.name.length() == 0 || u.chatId.length() == 0) {
    banner = "Name and Chat ID are required - user not added.";
    return;
  }
  if (!addTelegramUser(u)) {
    banner = "A Telegram user named \"" + htmlEscape(u.name) + "\" already exists - user not added.";
  }
}

void startWebServer(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  g_liveCameras = liveCameras;
  g_liveStates = liveStates;

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
    return response->send(200, "text/html", renderShell(Tab::Cameras, "", renderCamerasPanel()).c_str());
  });

  server.on("/add", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner;
    handleAddCamera(request, banner);
    if (banner.length() > 0) {
      return response->send(200, "text/html", renderShell(Tab::Cameras, banner, renderCamerasPanel()).c_str());
    }
    return response->redirect("/cameras");
  });

  server.on("/delete", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    deleteCamera(name);
    return response->redirect("/cameras");
  });

  server.on("/users", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Users, "", renderUsersPanel()).c_str());
  });

  server.on("/users/add", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner;
    handleAddUser(request, banner);
    if (banner.length() > 0) {
      return response->send(200, "text/html", renderShell(Tab::Users, banner, renderUsersPanel()).c_str());
    }
    return response->redirect("/users");
  });

  server.on("/users/delete", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    deleteTelegramUser(name);
    return response->redirect("/users");
  });

  server.begin();
  Serial.println("[WebServer] Camera management UI listening on port 80.");
}
