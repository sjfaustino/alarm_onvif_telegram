#include "webserver_cameras.h"
#include "format_utils.h"
#include "webserver_html.h"

// Finds cfg's matching live (currently-running) index by name, or -1 if
// this camera was added since the last reboot and isn't running yet, or was
// deleted and is still running until the next reboot.
static int findLiveCameraIndex(std::vector<CameraConfig>* liveCameras, const String& name) {
  if (!liveCameras) return -1;
  for (size_t i = 0; i < liveCameras->size(); i++) {
    if ((*liveCameras)[i].name.equalsIgnoreCase(name)) return (int)i;
  }
  return -1;
}

// Shared by "Add camera" (v = a fresh default CameraConfig), "Edit camera"
// (v = the stored record, password blanked), and a post-Test-Connection
// redisplay (v = whatever was just submitted). isEdit picks the legend/
// button text and whether a hidden originalName field is emitted.
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
  html += "<label>Snapshots per alert (1-10) - how many consecutive photos to send when motion "
          "fires, each a fresh fetch from the camera; raise it to see more of what led up to the alert"
          "<input type=\"text\" name=\"snapshotBurstCount\" value=\"" + String(v.snapshotBurstCount) +
          "\"></label>";
  html += "<label>Notes<input type=\"text\" name=\"notes\" value=\"" + htmlEscape(v.notes) + "\"></label>";
  html += "<p><button type=\"submit\" formaction=\"/cameras/save\">" +
          String(isEdit ? "Save changes" : "Add camera") + "</button> ";
  html += "<button type=\"submit\" formaction=\"/cameras/test\">Test Connection</button>";
  if (isEdit) html += " <a href=\"/cameras\">Cancel</a>";
  html += "</p></form></fieldset>";
  return html;
}

String renderCamerasPanel(const CameraConfig* prefill, bool isEdit,
                           std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  std::vector<CameraConfig> cams = loadCameras();

  String html = "<h1>Cameras</h1>";
  html += "<table><tr><th>Name</th><th>Device Service URL</th><th>Enabled</th>"
          "<th>Live Status</th><th>Last Alert</th><th>Notes</th><th></th></tr>";
  for (auto& c : cams) {
    int idx = findLiveCameraIndex(liveCameras, c.name);
    String liveStatus;
    String lastAlertStr = "never";
    if (idx >= 0 && liveStates && idx < (int)liveStates->size()) {
      // Read under lock - these fields are written by the camera's own
      // task, this render runs on PsychicHttp's task. See
      // CameraState::stateMutex.
      CameraState& st = (*liveStates)[idx];
      bool subscribed, offline, alertsEnabled, hasAlerted;
      uint32_t lastAlert;
      unsigned long revertDueMs;
      bool revertToOn;
      {
        CameraStateLock lock(st);
        subscribed = st.subscriptionActive;
        offline = st.isOffline;
        alertsEnabled = st.alertsEnabled;
        hasAlerted = st.hasAlerted;
        lastAlert = st.lastAlert;
        revertDueMs = st.scheduledRevertDueMs;
        revertToOn = st.scheduledRevertToOn;
      }
      liveStatus = subscribed ? "subscribed" : "not subscribed";
      if (offline) liveStatus += " - OFFLINE";
      if (!alertsEnabled) liveStatus += " (alerts OFF)";
      // (long) cast for the same overflow-safe "is this due yet" check
      // used everywhere else a millis() due-timestamp is compared - see
      // CameraState::scheduledRevertDueMs's comment.
      if (revertDueMs != 0 && (long)(millis() - revertDueMs) < 0) {
        liveStatus += " - auto " + String(revertToOn ? "ON" : "OFF") + " in " +
                      formatUptime(revertDueMs - millis());
      }
      if (hasAlerted) lastAlertStr = formatElapsedSince(lastAlert, millis());
    } else if (!c.enabled) {
      liveStatus = "disabled";
    } else {
      liveStatus = "not running - reboot to apply";
    }

    // Icon + native title-attribute tooltip instead of the full text, so a
    // long note doesn't blow out the column width - no JS needed, the
    // browser renders the tooltip on hover itself.
    String notesCell = c.notes.length() > 0
        ? "<span title=\"" + htmlEscape(c.notes) + "\" style=\"cursor:help;\">\xF0\x9F\x93\x9D</span>"
        : "";

    html += "<tr><td>" + htmlEscape(c.name) + "</td><td>" + htmlEscape(c.deviceServiceUrl) +
            "</td><td>" + (c.enabled ? "yes" : "no") + "</td><td>" + liveStatus + "</td><td>" +
            lastAlertStr + "</td><td>" +
            notesCell + "</td><td>";
    html += renderEditDeleteActions("/cameras/edit?name=", "/delete", c.name) + "</td></tr>";
  }
  html += "</table>";

  html += renderCameraForm(prefill ? *prefill : CameraConfig(), isEdit);

  html += "<p class=\"hint\">Adding, editing, or deleting a camera updates storage immediately, "
          "but only takes effect after the board reboots. Test Connection doesn't save anything - "
          "it just runs GetCapabilities/GetEventProperties/GetSnapshotUri against whatever is "
          "currently typed in, so you can catch a wrong URL or credential before rebooting.</p>";
  return html;
}

CameraConfig parseCameraForm(PsychicRequest* request) {
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

  long burstCount = request->getParam("snapshotBurstCount", "1").toInt();
  // Clamp to [1, 10]: blank/zero/negative falls back to 1 shot, and the
  // cap stops a fat-fingered number from flooding past Telegram's rate limit.
  if (burstCount < 1) burstCount = 1;
  if (burstCount > 10) burstCount = 10;
  c.snapshotBurstCount = (unsigned int)burstCount;

  return c;
}

bool saveCameraSubmission(CameraConfig cam, const String& originalName, String& banner) {
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

// Does create one real, temporary subscription on the camera (same as the
// real thing would) - not cleaned up afterward, so it just expires on its
// own.
//
// cfg.name/deviceServiceUrl are attacker-controllable the same way any other
// dashboard-submitted field is, and this banner - unlike every other one in
// this file - is built up from them directly rather than through a fixed
// message with a pre-escaped substitution, so both are run through
// htmlEscape() explicitly below. renderShell() (webserver.cpp) drops the
// returned banner straight into the page with no escaping of its own.
String testCameraConnection(CameraConfig cfg) {
  if (cfg.deviceServiceUrl.length() == 0) {
    return "Enter a device service URL first, then Test Connection.";
  }

  CameraState st;
  if (!resolveCameraCredentials(cfg, st)) {
    return "Enter a username and password first, then Test Connection.";
  }

  String safeName = htmlEscape(cfg.name);
  String safeUrl = htmlEscape(cfg.deviceServiceUrl);

  if (!cameraDiscoverServices(cfg, st)) {
    return "Test FAILED for \"" + safeName + "\": could not reach " + safeUrl +
           ", or no ONVIF event service was found there. Check the URL/credentials and see the "
           "Serial log for details.";
  }

  String result = "Test result for \"" + safeName + "\": device service reachable, event service found.";

  if (cameraGetEventServiceCapabilities(cfg, st) && cameraGetEventProperties(cfg, st)) {
    result += " Event service responds normally.";
  } else {
    result += " WARNING: the event service didn't respond to GetServiceCapabilities/GetEventProperties - "
              "this camera may not support ONVIF eventing at all.";
  }

  if (cameraCreatePullPoint(cfg, st)) {
    result += " Subscription created successfully.";
  } else {
    result += " WARNING: CreatePullPointSubscription failed - motion events won't be received even "
              "though the event service itself responds. See the Serial log for the SOAP fault; a "
              "camera that's had several failed subscription attempts recently (e.g. from repeated "
              "reboots) may just need time for old subscriptions to expire, or a power-cycle.";
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
