#include "webserver_users.h"
#include "camera_store.h"
#include "format_utils.h"

// Shared by "Add Telegram user" (v = a fresh TelegramUser with allCameras
// forced true, a friendlier default than the struct's own false), "Edit
// user" (v = the stored record), and a failed-save redisplay.
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
          String(v.canCommand ? " checked" : "") + "> May send /on, /off, /status, /uptime commands</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"canSnap\"" +
          String(v.canSnap ? " checked" : "") +
          "> May send /snap (on-demand photo) - independent of the commands above</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"canReset\"" +
          String(v.canReset ? " checked" : "") +
          "> May send /reset (reboots the board immediately) - independent of the permissions "
          "above, off by default even for a new user</label>";
  html += "<p><button type=\"submit\">" + String(isEdit ? "Save changes" : "Add user") + "</button>";
  if (isEdit) html += " <a href=\"/users\">Cancel</a>";
  html += "</p></form></fieldset>";
  return html;
}

String renderUsersPanel(const TelegramUser* prefill, bool isEdit) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  std::vector<CameraConfig> cams = loadCameras();

  String html = "<h1>Telegram Users</h1>";
  html += "<table><tr><th>Name</th><th>Chat ID</th><th>Cameras</th>"
          "<th>System Messages</th><th>Can Command</th><th>Can Snap</th><th>Can Reset</th><th></th></tr>";
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
            (u.canCommand ? "yes" : "no") + "</td><td>" + (u.canSnap ? "yes" : "no") + "</td><td>" +
            (u.canReset ? "yes" : "no") + "</td><td>";
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

TelegramUser parseUserForm(PsychicRequest* request) {
  TelegramUser u;
  u.name           = request->getParam("name", "");
  u.chatId         = request->getParam("chatId", "");
  u.allCameras     = request->hasParam("allCameras");
  u.systemMessages = request->hasParam("systemMessages");
  u.canCommand     = request->hasParam("canCommand");
  u.canSnap        = request->hasParam("canSnap");
  u.canReset       = request->hasParam("canReset");
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

bool saveUserSubmission(const TelegramUser& user, const String& originalName, String& banner) {
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
