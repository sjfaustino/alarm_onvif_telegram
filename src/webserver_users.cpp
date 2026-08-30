#include "webserver_users.h"
#include "camera_store.h"
#include "format_utils.h"
#include "webserver_html.h"
#include "telegram.h" // recentUnknownChats, sendTelegramMessage
#include "background_job.h" // BackgroundJob<T> - startTestMessageAsync
#include <freertos/FreeRTOS.h>

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
    html += renderEditDeleteActions("/users/edit?name=", "/users/delete", u.name) + "</td></tr>";
  }
  html += "</table>";

  html += "<form method=\"POST\" action=\"/users/test\">"
          "<p><button type=\"submit\">Send test message</button></p></form>";
  html += "<p class=\"hint\">Sends a real Telegram message, to every user above with "
          "\"Receive heartbeat and boot-online messages\" checked - the same audience a real boot "
          "notice/heartbeat/offline alert would reach. Confirms the bot token and TELEGRAM_ROOT_CA "
          "actually work before finding out the hard way when a real alert silently fails. Runs in "
          "the background; reload this page after clicking to see the result.</p>";
  html += renderTestMessageStatus();

  std::vector<UnknownChatSighting> unknownChats = recentUnknownChats();
  if (!unknownChats.empty()) {
    html += "<p>Recently messaged this bot, but not yet a configured user:</p>";
    std::vector<DiscoveryResultRow> rows;
    for (auto& sighting : unknownChats) {
      String chatIdStr = String((long long)sighting.chatId);
      rows.push_back({{chatIdStr, formatElapsedSince(sighting.lastSeenMs, millis())},
                       {{"prefillChatId", chatIdStr}}});
    }
    html += renderDiscoveryResultsTable({"Chat ID", "Last seen"}, "/users", rows);
    html += "<p class=\"hint\">Not a security log - just the last " + String((unsigned)UNKNOWN_CHAT_TRACK_MAX) +
            " distinct chat IDs that messaged the bot without matching a user below, for copy-paste "
            "convenience instead of a side trip to @userinfobot. Cleared on reboot.</p>";
  }

  TelegramUser blankAdd;
  blankAdd.allCameras = true; // friendlier default for a brand-new user than the struct's own false
  html += renderTelegramUserForm(prefill ? *prefill : blankAdd, cams, isEdit);

  html += "<p class=\"hint\">Adding, editing, or deleting a Telegram user takes effect on the next "
          "Telegram poll/alert - no reboot needed (unlike camera changes).</p>";
  return html;
}

// ============================================================
// Test message background wrapper - see webserver_users.h's own comment
// for why this can't run synchronously on the calling (PsychicHttp) task.
// BackgroundJob<T> (background_job.h) owns the mutex/state-machine part,
// same as webserver_cameras.cpp's identically-shaped "Test all cameras"/
// "Search network for cameras" wrappers.
// ============================================================

struct TestMessageResult {
  bool sent = false;         // sendTelegramMessage's own return - false can mean either case below
  size_t recipientCount = 0; // 0 means "false" above is "nobody has system messages on", not a send failure
};

static BackgroundJob<TestMessageResult> g_testMessageJob;

static void sendTestMessageTask(void*) {
  TestMessageResult r;
  for (auto& u : loadTelegramUsers()) {
    if (u.systemMessages) r.recipientCount++;
  }
  if (r.recipientCount > 0) {
    r.sent = sendTelegramMessage(
        "\xF0\x9F\xA7\xAA Test message from the Camera Monitor dashboard - if you're reading this, "
        "your Telegram setup is working.");
  }
  g_testMessageJob.finish(r);
  vTaskDelete(nullptr);
}

BackgroundJobStartOutcome startTestMessageAsync() {
  if (!g_testMessageJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one send at a time - a second click while one's in flight is a no-op

  // Same stack size as a real per-camera monitoring task (camera_tasks.h) -
  // this does the same WiFiClientSecure/HTTPClient TLS work a single SOAP
  // call would, just to api.telegram.org instead of a camera.
  BaseType_t created = xTaskCreate(sendTestMessageTask, "testMsg", 10240, nullptr, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // Without this, a task creation failure (out of memory) would leave
    // g_testMessageJob permanently stuck "in progress" - see
    // BackgroundJob<T>::cancelStart's own comment (background_job.h).
    g_testMessageJob.cancelStart();
    Serial.println("[webserver_users] ERROR: failed to start the test message task (out of memory?) - "
                    "try again once memory frees up.");
    return BackgroundJobStartOutcome::FailedToStart;
  }
  return BackgroundJobStartOutcome::Started;
}

String renderTestMessageStatus() {
  auto st = g_testMessageJob.status();
  if (st.inProgress) {
    return "<p class=\"hint\">Sending a test message in the background - reload this page in a "
           "moment to see the result. The rest of the dashboard stays responsive to everyone else "
           "in the meantime.</p>";
  }
  if (!st.hasResult) return "";
  if (st.result.recipientCount == 0) {
    return "<p class=\"hint\">No Telegram user has \"Receive heartbeat and boot-online messages\" "
           "enabled above - there was nobody to send a test to. Enable it for at least one user "
           "first.</p>";
  }
  return st.result.sent
      ? ("<p class=\"hint\">Test message sent to " + String((unsigned)st.result.recipientCount) +
         " user(s) with system messages enabled - check Telegram.</p>")
      : "<p class=\"hint\">Test message FAILED to send - see the Serial log (a bad bot token, or "
        "TELEGRAM_ROOT_CA in telegram_ca.h still being the placeholder, are the usual causes).</p>";
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
      banner = "Could not add \"" + htmlEscape(user.name) + "\" - a different user already uses that name "
               "or that Chat ID.";
      return false;
    }
    return true;
  }

  if (!updateTelegramUser(originalName, user)) {
    banner = "Could not save \"" + htmlEscape(user.name) + "\" - a different user already uses that name "
             "or that Chat ID.";
    return false;
  }
  return true;
}
