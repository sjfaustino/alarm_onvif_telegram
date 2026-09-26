#include "user_form.h"
#include "config.h"
#include "format_utils.h"

// Shared by "Add Telegram user" (v = a fresh TelegramUser with allCameras
// forced true, a friendlier default than the struct's own false), "Edit
// user" (v = the stored record), and a failed-save redisplay.
String renderTelegramUserForm(const TelegramUser& v, const std::vector<CameraConfig>& cams, bool isEdit) {
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
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"canBackup\"" +
          String(v.canBackup ? " checked" : "") +
          "> May send /backup (sends the full config export as a file - includes every camera's own "
          "username/password, though never the WiFi password) - independent of the permissions "
          "above, off by default even for a new user</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"canRestore\"" +
          String(v.canRestore ? " checked" : "") +
          "> May send /restore (uploads a config export file back to the bot and applies it, "
          "overwriting cameras/users/network settings) - the single most disruptive command available, "
          "independent of the permissions above, off by default even for a new user</label>";
  html += "<label>Max commands per minute (0 = unlimited)"
          "<input type=\"text\" name=\"maxCommandsPerMinute\" value=\"" + String(v.maxCommandsPerMinute) +
          "\"></label>";
  bool isPt = v.language == TelegramLang::Portuguese;
  html += "<label>Language (alerts and command replies sent to this user - the dashboard itself always "
          "stays English)<select name=\"language\">"
          "<option value=\"en\"" + String(!isPt ? " selected" : "") + ">English</option>"
          "<option value=\"pt\"" + String(isPt ? " selected" : "") + ">Portugu\xC3\xAAs</option>"
          "</select></label>";
  html += "<p><button type=\"submit\">" + String(isEdit ? "Save changes" : "Add user") + "</button>";
  if (isEdit) html += " <a href=\"/users\" class=\"secondary\">Cancel</a>";
  html += "</p></form></fieldset>";
  return html;
}

TelegramUser parseUserForm(const FormParams& params, const std::vector<CameraConfig>& cams) {
  TelegramUser u;
  u.name           = params.get("name", "");
  u.chatId         = params.get("chatId", "");
  u.allCameras     = params.has("allCameras");
  u.systemMessages = params.has("systemMessages");
  u.canCommand     = params.has("canCommand");
  u.canSnap        = params.has("canSnap");
  u.canReset       = params.has("canReset");
  u.canBackup      = params.has("canBackup");
  u.canRestore     = params.has("canRestore");

  // 0 is the deliberate, meaningful "unlimited" value - never substitute
  // it away, only clamp a negative (not reachable from a plain number
  // input, but defensive) or oversized value, same reasoning as
  // CameraConfig's motionWatchdogHours/timelapseIntervalMin fields.
  long maxCommandsPerMinute = params.get("maxCommandsPerMinute", "0").toInt();
  if (maxCommandsPerMinute < 0) maxCommandsPerMinute = 0;
  if (maxCommandsPerMinute > (long)TELEGRAM_MAX_COMMANDS_PER_MINUTE_MAX) {
    maxCommandsPerMinute = (long)TELEGRAM_MAX_COMMANDS_PER_MINUTE_MAX;
  }
  u.maxCommandsPerMinute = (uint16_t)maxCommandsPerMinute;

  u.language = params.get("language", "en") == "pt" ? TelegramLang::Portuguese : TelegramLang::English;

  u.name.trim();
  u.chatId.trim();

  if (!u.allCameras) {
    for (auto& c : cams) {
      if (params.has(("cam_" + c.name).c_str())) {
        u.cameraNames.push_back(c.name);
      }
    }
  }
  return u;
}
