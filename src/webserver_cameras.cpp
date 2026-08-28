#include "webserver_cameras.h"
#include "format_utils.h"
#include "webserver_html.h"
#include "camera_tasks.h"
#include "event_log_store.h"
#include "snapshot_history.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cctype>

// Serializes saveCameraSubmission's whole "decide whether this camera was
// already running, then apply live or note a reboot's needed" section
// below - PsychicHttp can run more than one request concurrently, and
// without this, two near-simultaneous saves of the same *newly-enabled*
// camera could both observe wasRunning==false (cam.enabled in
// liveCameras only flips once the spawned task's own applyPendingConfigIfAny
// actually runs, which is asynchronous - not immediately when
// spawnCameraTask returns) and both call spawnCameraTask for the same
// slot, leaving two tasks fighting over one CameraConfig/CameraState.
// Camera saves are a low-frequency, admin-driven action, not a hot path -
// serializing all of them (even ones for different cameras) costs nothing
// worth avoiding a per-camera locking scheme for.
static SemaphoreHandle_t g_saveMutex = xSemaphoreCreateMutex();

// Formats minutes-since-midnight as "HH:MM" for pre-filling an
// <input type="time"> value attribute.
static String minutesToHHMM(uint16_t minutes) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(minutes / 60) % 24, (unsigned)(minutes % 60));
  return String(buf);
}

// Parses an <input type="time"> value ("HH:MM", 24h) into minutes since
// midnight - NOT the same as this file's other numeric fields' plain
// `.toInt()` (that would silently stop at the colon and drop the
// minutes). Same length/digit/range validation parseDurationToken's own
// HH:MM branch (lib/telegram_parse) already uses, duplicated here rather
// than shared - different file/purpose, not worth a shared lib for one
// call site. Returns 0 (midnight) on anything malformed, matching
// isWithinQuietHours' own "0/0 means no active window" safe default.
static uint16_t parseHHMMToMinutes(const String& hhmm) {
  if (hhmm.length() != 5 || hhmm[2] != ':') return 0;
  for (int i = 0; i < 5; i++) {
    if (i == 2) continue;
    if (!isdigit((unsigned char)hhmm[i])) return 0;
  }
  int h = hhmm.substring(0, 2).toInt();
  int m = hhmm.substring(3, 5).toInt();
  if (h > 23 || m > 59) return 0;
  return (uint16_t)(h * 60 + m);
}

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
  html += "<label>Alert cooldown, seconds, max 86400 (minimum time between Telegram alerts for this camera)"
          "<input type=\"text\" name=\"alertCooldownSec\" value=\"" + String(v.alertCooldownMs / 1000) +
          "\"></label>";
  html += "<label>Offline threshold, minutes, max 10080 (no response for this long -> OFFLINE alert)"
          "<input type=\"text\" name=\"offlineThresholdMin\" value=\"" + String(v.offlineThresholdMs / 60000UL) +
          "\"></label>";
  html += "<label>Snapshots per alert (1-10) - how many consecutive photos to send when motion "
          "fires, each a fresh fetch from the camera; raise it to see more of what led up to the alert"
          "<input type=\"text\" name=\"snapshotBurstCount\" value=\"" + String(v.snapshotBurstCount) +
          "\"></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"quietHoursEnabled\"" +
          String(v.quietHoursEnabled ? " checked" : "") +
          "> Quiet hours (mutes motion alerts only - tamper/offline still alert)</label>";
  html += "<label>Quiet hours start<input type=\"time\" name=\"quietStart\" value=\"" +
          minutesToHHMM(v.quietStartMinute) + "\"></label>";
  html += "<label>Quiet hours end<input type=\"time\" name=\"quietEnd\" value=\"" +
          minutesToHHMM(v.quietEndMinute) +
          "\"></label><p class=\"hint\">Leaving start and end the same (e.g. both 00:00) means no "
          "active window - quiet hours needs a real start/end to do anything.</p>";
  html += "<label>No-motion watchdog, hours (0 = off) - alerts if this camera hasn't seen ANY motion "
          "in over this long, e.g. a dead PIR or a knocked-over camera"
          "<input type=\"text\" name=\"motionWatchdogHours\" value=\"" + String(v.motionWatchdogHours) +
          "\"></label>";
  // Motion still updates the watchdog's clock during quiet hours (it's
  // only the Telegram *send* that's suppressed - see triggerMotionAlert),
  // so a watchdog window shorter than the quiet-hours window will
  // legitimately trip on a camera that's working perfectly fine, purely
  // because it hasn't seen motion during a stretch the user themselves
  // configured as expected-quiet. Two independently-configured settings
  // with no other cross-validation between them - flagged here rather
  // than silently left for the user to discover as a confusing false alert.
  if (v.quietHoursEnabled && v.quietStartMinute != v.quietEndMinute && v.motionWatchdogHours > 0) {
    int windowMin = (v.quietEndMinute > v.quietStartMinute)
        ? (v.quietEndMinute - v.quietStartMinute)
        : (1440 - v.quietStartMinute + v.quietEndMinute); // wraps past midnight
    if ((int)v.motionWatchdogHours * 60 <= windowMin) {
      html += "<p class=\"hint\">\xE2\x9A\xA0\xEF\xB8\x8F The no-motion watchdog (" +
              String(v.motionWatchdogHours) + "h) is shorter than or equal to the quiet hours window (" +
              String(windowMin / 60) + "h" + String(windowMin % 60) + "m) - it will likely trip a false "
              "alert every quiet period even though nothing's actually wrong. Consider raising the "
              "watchdog hours above the quiet hours window length.</p>";
    }
  }
  html += "<label>Timelapse capture, minutes (0 = off) - stores a snapshot on this interval "
          "regardless of motion (never sent to Telegram, just kept in history/SD)"
          "<input type=\"text\" name=\"timelapseIntervalMin\" value=\"" + String(v.timelapseIntervalMin) +
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
          "<th>Live Status</th><th>Last Alert</th><th>Preview</th><th>Notes</th><th></th></tr>";
  for (auto& c : cams) {
    int idx = findLiveCameraIndex(liveCameras, c.name);
    String liveStatus;
    String lastAlertStr = "never";
    String previewCell = "<span class=\"hint\">(none yet)</span>";
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

      // cameraSnapshotCount/the "age" param dispatch to whichever backing
      // store is active (SD or the PSRAM ring) - see snapshot_history.h.
      size_t historyCount = cameraSnapshotCount(c, st);
      if (historyCount > 0) {
        // Newest first (age 0). One shared render-time value cache-busts
        // every thumbnail on this page load - simpler than a per-entry
        // key, and just as effective: a full page reload always gets a
        // fresh renderMs, so the browser never shows a stale image across
        // page loads, which is all this needs to guarantee.
        unsigned long renderMs = millis();
        previewCell = "";
        for (size_t age = 0; age < historyCount; age++) {
          String url = "/cameras/snapshot?name=" + urlEncode(c.name) + "&age=" + String((unsigned)age) +
                        "&t=" + String(renderMs);
          previewCell += "<a href=\"" + url + "\" target=\"_blank\">"
                         "<img src=\"" + url + "\" style=\"max-width:48px;max-height:36px;margin:1px;\" "
                         "alt=\"preview\"></a>";
        }
      }
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
            lastAlertStr + "</td><td>" + previewCell + "</td><td>" +
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
  // Upper-capped at 24h (86400s): unsigned long is 32-bit on this platform,
  // and *1000UL overflows/wraps above ~4,294,967s - a fat-fingered huge
  // cooldown would otherwise silently wrap into a tiny one (alert spam
  // instead of the throttling actually requested), same overflow class
  // motionWatchdogHours/timelapseIntervalMin below are already clamped
  // against.
  if (cooldownSec > 86400) cooldownSec = 86400;
  c.alertCooldownMs = cooldownSec > 0 ? (unsigned long)cooldownSec * 1000UL : CameraConfig().alertCooldownMs;

  long offlineMin = request->getParam("offlineThresholdMin", "5").toInt();
  // Upper-capped at 7 days (10080min), same overflow reasoning as above -
  // *60000UL wraps above ~71583 minutes.
  if (offlineMin > 10080) offlineMin = 10080;
  c.offlineThresholdMs = offlineMin > 0 ? (unsigned long)offlineMin * 60000UL : CameraConfig().offlineThresholdMs;

  long burstCount = request->getParam("snapshotBurstCount", "1").toInt();
  // Clamp to [1, 10]: blank/zero/negative falls back to 1 shot, and the
  // cap stops a fat-fingered number from flooding past Telegram's rate limit.
  if (burstCount < 1) burstCount = 1;
  if (burstCount > 10) burstCount = 10;
  c.snapshotBurstCount = (unsigned int)burstCount;

  c.quietHoursEnabled = request->hasParam("quietHoursEnabled");
  c.quietStartMinute  = parseHHMMToMinutes(request->getParam("quietStart", "00:00"));
  c.quietEndMinute    = parseHHMMToMinutes(request->getParam("quietEnd", "00:00"));

  // Unlike alertCooldownSec/offlineThresholdMin/snapshotBurstCount above,
  // 0 is the deliberate, meaningful "off" value for both of these fields -
  // it must never be substituted away, only clamped against a negative
  // value (not reachable from a plain number input, but defensive) and an
  // upper sanity cap.
  long watchdogHours = request->getParam("motionWatchdogHours", "0").toInt();
  if (watchdogHours < 0) watchdogHours = 0;
  if (watchdogHours > 168) watchdogHours = 168; // 1 week
  c.motionWatchdogHours = (uint16_t)watchdogHours;

  long timelapseMin = request->getParam("timelapseIntervalMin", "0").toInt();
  if (timelapseMin < 0) timelapseMin = 0;
  if (timelapseMin > 1440) timelapseMin = 1440; // 24h
  c.timelapseIntervalMin = (uint16_t)timelapseMin;

  return c;
}

bool saveCameraSubmission(CameraConfig cam, const String& originalName, String& banner, String& applyNote,
                           std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  if (cam.name.length() == 0 || cam.deviceServiceUrl.length() == 0) {
    banner = "Name and device service URL are required - camera not saved.";
    return false;
  }

  if (originalName.length() == 0) {
    if (!addCamera(cam)) {
      banner = "A camera named \"" + htmlEscape(cam.name) + "\" already exists - camera not added.";
      return false;
    }
    // A brand new camera has no slot in liveCameras/liveStates at all yet
    // (those are sized once at boot and never grow) - still needs a
    // reboot before it can be monitored, same as always. Nothing to note.
    return true;
  }

  if (cam.pass.length() == 0) {
    for (auto& existing : loadCameras()) {
      if (existing.name.equalsIgnoreCase(originalName)) { cam.pass = existing.pass; break; }
    }
  }

  // Everything from here on - deciding wasRunning, persisting to NVS, and
  // applying (or not) a live reload - runs under g_saveMutex: see its own
  // comment for why (double-spawn prevention for two near-simultaneous
  // saves of the same camera). Incidentally also serializes concurrent
  // saves of two *different* cameras against updateCamera/addCamera's own
  // read-all-modify-one-save-all pattern (camera_store.cpp), which had no
  // such protection of its own before this.
  xSemaphoreTake(g_saveMutex, portMAX_DELAY);

  // Captured BEFORE updateCamera touches NVS, using the ORIGINAL name - a
  // rename doesn't change which live slot this is. wasRunning reflects
  // reality precisely because nothing except spawnCameraTask() (main.cpp)
  // ever flips a live camera's task into existence, and it's only ever
  // called when enabled was true at boot or via the live-spawn path below
  // - so "was this slot's in-memory CameraConfig::enabled true" is
  // exactly "does a task exist for it right now".
  int idx = findLiveCameraIndex(liveCameras, originalName);
  bool wasRunning = (idx >= 0) && liveStates && idx < (int)liveStates->size() && (*liveCameras)[idx].enabled;

  if (!updateCamera(originalName, cam)) {
    xSemaphoreGive(g_saveMutex);
    banner = "Could not save \"" + htmlEscape(cam.name) +
             "\" - a different camera already uses that name.";
    return false;
  }

  if (idx >= 0 && liveStates && idx < (int)liveStates->size()) {
    // Deliberately NOT htmlEscape()d here - applyNote rides through a
    // URL-encoded redirect query param (see webserver.cpp's /cameras/save
    // and /cameras GET handlers) and is escaped exactly once, at the
    // single point it's actually rendered into HTML (the /cameras GET
    // handler). Escaping it here too used to seem like defense in depth,
    // but it actually created a real reflected-XSS hole: the GET handler
    // trusted that pre-escaping and skipped its own, which meant a direct
    // request to /cameras?note=<script>...</script> (bypassing this POST
    // flow entirely) rendered completely unescaped. One escaping point,
    // not zero and not two.
    if (wasRunning && cam.enabled) {
      // Still enabled before and after - stage the new config for the
      // already-running task to pick up itself. See requestLiveConfigReload
      // (camera.h) for why this webserver task must never write the live
      // CameraConfig's String fields directly.
      requestLiveConfigReload((*liveStates)[idx], cam);
      applyNote = "\"" + cam.name + "\" updated - applying live, reconnecting now (no reboot needed).";
    } else if (!wasRunning && cam.enabled) {
      // Was disabled (or simply never got a task at boot) and this edit
      // just enabled it. Staged via the same pendingConfig mechanism a
      // running camera's edit uses, NOT written into (*liveCameras)[idx]
      // directly from here - even though no task owns this slot yet,
      // CameraConfig itself has no locking of its own, and other tasks
      // (heartbeat, /status, this same dashboard) may read
      // cameras[i].enabled/.name for it without a lock the instant it
      // flips to enabled. cameraStateInit() first, so that lock actually
      // means something (a never-spawned camera's mutex doesn't exist
      // yet) - see requestLiveConfigReload's comment.
      cameraStateInit((*liveStates)[idx]);
      requestLiveConfigReload((*liveStates)[idx], cam);
      spawnCameraTask(idx); // its own startup applies the staged config - see applyPendingConfigIfAny
      logEvent(cam.name + ": enabled via dashboard, monitoring started live");
      applyNote = "\"" + cam.name + "\" enabled - monitoring started live (no reboot needed).";
    } else if (wasRunning && !cam.enabled) {
      // Every other field change here (if any) still needs a reboot to
      // take effect too - there's no live task-teardown path yet, so
      // rather than apply a half-edit, this whole save waits for a
      // reboot, exactly like it always has for a disable.
      applyNote = "\"" + cam.name + "\" disabled, but its task is still running - reboot to fully stop it.";
    }
    // else: wasn't running, still not enabled - nothing live to do.
  }
  // else: idx < 0 - this camera isn't in liveCameras at all (added to NVS
  // after this board's current boot) - still needs a reboot to get a live
  // slot in the first place, same as always.

  xSemaphoreGive(g_saveMutex);
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
