#include "webserver_cameras.h"
#include "telegram.h" // sendTestAlert
#include "config.h" // CAMERA_ALERT_COOLDOWN_MAX_MS/CAMERA_OFFLINE_THRESHOLD_MAX_MS/CAMERA_SNAPSHOT_BURST_MAX/CAMERA_SNAPSHOT_DIMENSION_MAX
#include "format_utils.h"
#include "webserver_html.h"
#include "camera_tasks.h"
#include "event_log_store.h"
#include "snapshot_history.h"
#include "snapshot_source.h" // snapshotSourceLabel - Preview column thumbnail tooltips
#include "onvif_soap.h" // makeUUID, for the discovery Probe's MessageID
#include "background_job.h" // BackgroundJob<T>, shared by the test-all and discovery buttons below
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <cctype>

// Serializes saves: two quick saves of a newly enabled camera could both see
// it not running (enabled flips only when the task applies its config) and
// spawn two tasks.
static SemaphoreHandle_t g_saveMutex = xSemaphoreCreateMutex();

static String minutesToHHMM(uint16_t minutes) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(minutes / 60) % 24, (unsigned)(minutes % 60));
  return String(buf);
}

// "HH:MM" -> minutes since midnight (toInt() would stop at the colon). 0 on
// malformed input, which quiet hours treat as no window.
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

// Live slot index by name, or -1 if the camera has no live slot.
static int findLiveCameraIndex(std::vector<CameraConfig>* liveCameras, const String& name) {
  if (!liveCameras) return -1;
  for (size_t i = 0; i < liveCameras->size(); i++) {
    if ((*liveCameras)[i].name.equalsIgnoreCase(name)) return (int)i;
  }
  return -1;
}

// Embeds URL-encoded user:pass as userinfo so VLC/an NVR/the browser can use
// the link directly (the same credentials already in NVS). Leaves a URI that
// already has userinfo, or no username, unchanged.
static String buildUriWithCredentials(const String& rawUri, const String& user, const String& pass) {
  int schemeEnd = rawUri.indexOf("://");
  if (schemeEnd < 0 || user.length() == 0) return rawUri;
  int afterScheme = schemeEnd + 3;
  int nextSlash = rawUri.indexOf('/', afterScheme);
  String authority = (nextSlash >= 0) ? rawUri.substring(afterScheme, nextSlash) : rawUri.substring(afterScheme);
  if (authority.indexOf('@') >= 0) return rawUri; // already has credentials embedded
  return rawUri.substring(0, afterScheme) + urlEncode(user) + ":" + urlEncode(pass) + "@" +
         rawUri.substring(afterScheme);
}

// Add (fresh defaults), Edit (stored record, password blanked), or a redisplay
// after Test Connection.
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
  html += "<label>Snapshot URI override (optional; {USER}/{PASS}/{WIDTH}/{HEIGHT} substituted at runtime)"
          "<input type=\"text\" name=\"snapshotUriOverride\" value=\"" +
          htmlEscape(v.snapshotUriOverride) + "\"></label>";
  html += "<label>Snapshot width override, for {WIDTH} above (optional, max " +
          String(CAMERA_SNAPSHOT_DIMENSION_MAX) + ", blank/0 = unset)"
          "<input type=\"text\" name=\"snapshotMaxWidth\" value=\"" +
          (v.snapshotMaxWidth > 0 ? String(v.snapshotMaxWidth) : String("")) + "\"></label>";
  html += "<label>Snapshot height override, for {HEIGHT} above (optional, max " +
          String(CAMERA_SNAPSHOT_DIMENSION_MAX) + ", blank/0 = unset)"
          "<input type=\"text\" name=\"snapshotMaxHeight\" value=\"" +
          (v.snapshotMaxHeight > 0 ? String(v.snapshotMaxHeight) : String("")) + "\"></label>";
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
  html += "<label>Poll interval, ms, " + String(CAMERA_POLL_INTERVAL_MIN_MS) + "-" +
          String(CAMERA_POLL_INTERVAL_MAX_MS) + " (how often this camera is asked \"anything new?\" - "
          "lower notices motion sooner, at the cost of more frequent requests to this camera; some "
          "cheaper cameras' embedded HTTP stacks tolerate that worse than others)"
          "<input type=\"text\" name=\"pollIntervalMs\" value=\"" + String(v.pollIntervalMs) +
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
  // Motion during quiet hours still feeds the watchdog clock, so a watchdog
  // shorter than the quiet window would false-alarm; warn about the combo.
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
          "regardless of motion, kept in history/SD"
          "<input type=\"text\" name=\"timelapseIntervalMin\" value=\"" + String(v.timelapseIntervalMin) +
          "\"></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"timelapseSendToTelegram\"" +
          String(v.timelapseSendToTelegram ? " checked" : "") +
          "> Also send these scheduled snapshots to Telegram (in addition to storing them) - "
          "ignored while the interval above is 0</label>";
  html += "<label>Snapshot retention override, days (0 = use the Storage page's global setting)"
          "<input type=\"text\" name=\"retentionDays\" value=\"" + String(v.retentionDays) + "\"></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"personAlertsEnabled\"" +
          String(v.personAlertsEnabled ? " checked" : "") +
          "> Alert on person detection - on by default; only uncheck to mute person alerts for this "
          "specific camera while leaving other detection types alone (requires a camera whose own "
          "ONVIF AI actually distinguishes person detection - a plain motion sensor always alerts "
          "regardless of this)</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"vehicleAlertsEnabled\"" +
          String(v.vehicleAlertsEnabled ? " checked" : "") +
          "> Alert on vehicle detection - on by default; uncheck for a camera facing a busy street "
          "that would otherwise page you for every passing car</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"petAlertsEnabled\"" +
          String(v.petAlertsEnabled ? " checked" : "") +
          "> Alert on pet (dog/cat) detection too - off by default, since a person/vehicle-only "
          "camera would otherwise page you for your own pet</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"petAlertsTextOnly\"" +
          String(v.petAlertsTextOnly ? " checked" : "") +
          "> Send pet alerts as text only, no photo - ignored unless pet alerts above are on</label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"motionDigestEnabled\"" +
          String(v.motionDigestEnabled ? " checked" : "") +
          "> Send a follow-up \"motion continued\" summary after a real alert's cooldown ends - on by "
          "default; uncheck for a camera busy enough that the follow-up summary is itself noise on top "
          "of the alert it's summarizing</label>";
  html += "<label>Notes<input type=\"text\" name=\"notes\" value=\"" + htmlEscape(v.notes) + "\"></label>";
  html += "<p><button type=\"submit\" formaction=\"/cameras/save\">" +
          String(isEdit ? "Save changes" : "Add camera") + "</button> ";
  html += "<button type=\"submit\" formaction=\"/cameras/test\">Test Connection</button>";
  if (isEdit) html += " <a href=\"/cameras\" class=\"secondary\">Cancel</a>";
  html += "</p></form></fieldset>";
  return html;
}

// One-line health summary above the table, using the same classification as
// the rows. Nothing for zero cameras.
static String renderCameraHealthSummary(const std::vector<CameraConfig>& cams,
                                         std::vector<CameraConfig>* liveCameras,
                                         std::vector<CameraState>* liveStates) {
  if (cams.empty()) return "";

  size_t disabledCount = 0, notRunningCount = 0, onlineCount = 0, notSubscribedCount = 0,
         offlineCount = 0, mutedCount = 0;
  unsigned long nowMs = millis();
  bool haveLastMotion = false;
  unsigned long lastMotionAgoMs = 0;
  String lastMotionCamera;

  for (auto& c : cams) {
    if (!c.enabled) { disabledCount++; continue; }

    int idx = findLiveCameraIndex(liveCameras, c.name);
    bool isLive = idx >= 0 && liveStates && idx < (int)liveStates->size();
    if (!isLive) { notRunningCount++; continue; } // enabled, but no reboot yet to spawn its task

    CameraState& st = (*liveStates)[idx];
    bool offline, subscribed, alertsEnabled, hasAlerted;
    uint32_t lastAlert;
    {
      CameraStateLock lock(st);
      offline = st.isOffline;
      subscribed = st.subscriptionActive;
      alertsEnabled = st.alertsEnabled;
      hasAlerted = st.hasAlerted;
      lastAlert = st.lastAlert;
    }
    if (offline) offlineCount++;
    else if (subscribed) onlineCount++;
    else notSubscribedCount++;
    if (!alertsEnabled) mutedCount++; // independent of the three above - a muted camera can be any of them

    if (hasAlerted) {
      unsigned long agoMs = nowMs - lastAlert;
      if (!haveLastMotion || agoMs < lastMotionAgoMs) {
        haveLastMotion = true;
        lastMotionAgoMs = agoMs;
        lastMotionCamera = c.name;
      }
    }
  }

  String html = "<p>";
  if (onlineCount > 0) {
    html += "<span class=\"badge badge-on\">" + String((unsigned)onlineCount) + " online</span> ";
  }
  if (offlineCount > 0) {
    html += "<span class=\"badge badge-offline\">" + String((unsigned)offlineCount) + " offline</span> ";
  }
  if (notSubscribedCount > 0) {
    html += "<span class=\"badge badge-warn\">" + String((unsigned)notSubscribedCount) + " not subscribed</span> ";
  }
  if (notRunningCount > 0) {
    html += "<span class=\"badge badge-warn\">" + String((unsigned)notRunningCount) +
            " not running (reboot needed)</span> ";
  }
  if (disabledCount > 0) {
    html += "<span class=\"badge badge-off\">" + String((unsigned)disabledCount) + " disabled</span> ";
  }
  if (mutedCount > 0) {
    html += "<span class=\"badge badge-off\">" + String((unsigned)mutedCount) + " muted</span> ";
  }
  if (haveLastMotion) {
    html += "&mdash; last motion: " + htmlEscape(lastMotionCamera) + ", " +
            formatElapsedSince(nowMs - lastMotionAgoMs, nowMs);
  }
  html += "</p>";
  return html;
}

String renderCamerasPanel(const CameraConfig* prefill, bool isEdit,
                           std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  std::vector<CameraConfig> cams = loadCameras();

  String html = "<h1>Cameras</h1>";
  html += renderCameraHealthSummary(cams, liveCameras, liveStates);
  html += "<table><tr><th>Name</th><th>Device Service URL</th><th>Enabled</th>"
          "<th>Live Status</th><th>Last Alert</th><th>Preview</th><th>Notes</th><th></th></tr>";
  size_t rowIdx = 0; // unique per-row DOM id source for the latency toggle below - findLiveCameraIndex's
                      // own idx can be -1 (camera with no live slot)
  for (auto& c : cams) {
    int idx = findLiveCameraIndex(liveCameras, c.name);
    String liveStatus;
    String lastAlertStr = "never";
    String previewCell = "<span class=\"hint\">(none yet)</span>";
    String rtspUri;  // populated below only if isLive - see CameraState::streamUri's own comment
    String mjpegUri; // populated below only if isLive - see CameraState::mjpegUri's own comment
    String liveRowHtml; // set below only if mjpegUri is non-empty - a sibling <tr> appended after this row's own
    // c.enabled (fresh from NVS) too: a stopped camera keeps its slot, so idx
    // alone can't tell a live task from a finished one.
    bool isLive = idx >= 0 && liveStates && idx < (int)liveStates->size() && c.enabled;
    if (isLive) {
      // Written by the camera task; read under the lock.
      CameraState& st = (*liveStates)[idx];
      bool subscribed, offline, alertsEnabled, hasAlerted;
      uint32_t lastAlert;
      unsigned long revertDueMs, totalReconnects;
      bool revertToOn;
      size_t recentReconnects = 0; // how many of reconnectHistory's entries fall in the last 24h
      size_t recentOfflineEvents = 0; // same, for offlineHistory
      size_t latencyCount = 0;
      unsigned long latencySum = 0, latencyMin = 0, latencyMax = 0;
      unsigned long nowMs = millis();
      String supportedEventTopics;
      {
        CameraStateLock lock(st);
        subscribed = st.subscriptionActive;
        offline = st.isOffline;
        alertsEnabled = st.alertsEnabled;
        hasAlerted = st.hasAlerted;
        lastAlert = st.lastAlert;
        revertDueMs = st.scheduledRevertDueMs;
        revertToOn = st.scheduledRevertToOn;
        totalReconnects = st.totalReconnects;
        rtspUri = st.streamUri;
        mjpegUri = st.mjpegUri;
        supportedEventTopics = st.supportedEventTopics;
        for (size_t i = 0; i < st.reconnectHistoryCount; i++) {
          if (nowMs - st.reconnectHistory[i] < 24UL * 3600UL * 1000UL) recentReconnects++;
        }
        for (size_t i = 0; i < st.offlineHistoryCount; i++) {
          if (nowMs - st.offlineHistory[i] < 24UL * 3600UL * 1000UL) recentOfflineEvents++;
        }
        latencyCount = st.motionLatencyHistoryCount;
        for (size_t i = 0; i < latencyCount; i++) {
          unsigned long v = st.motionLatencyHistory[i];
          latencySum += v;
          if (i == 0 || v < latencyMin) latencyMin = v;
          if (v > latencyMax) latencyMax = v;
        }
      }
      // OFFLINE (not answering), NOT SUBSCRIBED (answering but no
      // subscription) and ONLINE are different problems, hence three states.
      if (offline) {
        liveStatus = "<span class=\"badge badge-offline\">OFFLINE</span>";
      } else if (subscribed) {
        liveStatus = "<span class=\"badge badge-on\">ONLINE</span>";
      } else {
        liveStatus = "<span class=\"badge badge-warn\">NOT SUBSCRIBED</span>";
      }
      // Supported detection types as a hover tooltip.
      if (supportedEventTopics.length() > 0) {
        liveStatus += " <span title=\"Event schema mentions: " + htmlEscape(supportedEventTopics) +
                      "\">\xE2\x84\xB9\xEF\xB8\x8F</span>";
      }
      // Shown even when online again - repeated flapping is what it reveals.
      if (recentOfflineEvents > 0) {
        liveStatus += " - went offline " + String((unsigned)recentOfflineEvents) +
                      (recentOfflineEvents == EVENT_HISTORY_RING_SIZE ? "+" : "") + " time(s) in the last 24h";
      }
      if (!alertsEnabled) liveStatus += " <span class=\"badge badge-off\">MUTED</span>";
      if (revertDueMs != 0 && (long)(millis() - revertDueMs) < 0) {
        liveStatus += " - auto " + String(revertToOn ? "ON" : "OFF") + " in " +
                      formatUptime(revertDueMs - millis());
      }
      if (totalReconnects > 0) {
        liveStatus += " - " + String(totalReconnects) + " reconnect(s) since boot";
        // "+" means a floor: every ring slot is within 24h, so older ones may
        // have been evicted.
        if (recentReconnects > 0) {
          liveStatus += " (" + String((unsigned)recentReconnects) +
                        (recentReconnects == EVENT_HISTORY_RING_SIZE ? "+" : "") + " in the last 24h)";
        }
      }
      // Latency detail behind a click (vanilla JS toggle); it's tuning info.
      if (latencyCount > 0) {
        unsigned long avgMs = latencySum / latencyCount;
        String latencyId = "lat" + String((unsigned)rowIdx);
        liveStatus += " <a href=\"#\" onclick=\"var d=document.getElementById('" + latencyId +
                      "');d.style.display=(d.style.display==='inline')?'none':'inline';return false;\" "
                      "title=\"Motion-to-photo latency\">&#9201;</a>";
        // Poll interval alongside, for comparison.
        liveStatus += "<span id=\"" + latencyId + "\" style=\"display:none;\"> - last " +
                      String((unsigned)latencyCount) + ": avg " + String(avgMs) + "ms, min " +
                      String(latencyMin) + "ms, max " + String(latencyMax) + "ms (poll interval " +
                      String(c.pollIntervalMs) + "ms)</span>";
      }
      if (hasAlerted) lastAlertStr = formatElapsedSince(lastAlert, millis());

      size_t historyCount = cameraSnapshotCount(c, st);
      if (historyCount > 0) {
        // Newest first. One render-time value cache-busts the thumbnails.
        unsigned long renderMs = millis();
        previewCell = "";
        // Oldest-to-newest for the flipbook (age counts back from newest).
        String flipbookUrls;
        for (size_t age = 0; age < historyCount; age++) {
          String url = "/cameras/snapshot?name=" + urlEncode(c.name) + "&age=" + String((unsigned)age) +
                        "&t=" + String(renderMs);
          // Source as a tooltip; the strip has no room for captions.
          String sourceLabel = snapshotSourceLabel(cameraSnapshotSourceAt(c, st, age));
          previewCell += "<a href=\"" + url + "\" target=\"_blank\">"
                         "<img src=\"" + url + "\" style=\"max-width:48px;max-height:36px;margin:1px;\" "
                         "alt=\"preview\" title=\"" + sourceLabel + "\"></a>";
          String oldestFirstUrl = "/cameras/snapshot?name=" + urlEncode(c.name) +
                                   "&age=" + String((unsigned)(historyCount - 1 - age)) + "&t=" + String(renderMs);
          flipbookUrls += (age > 0 ? "|" : "") + oldestFirstUrl;
        }
        // Flipbook reuses the loaded thumbnails (no extra requests).
        if (historyCount > 1) {
          previewCell += "<br><button type=\"button\" class=\"secondary\" "
                         "onclick=\"playFlipbook(this,'" + flipbookUrls + "')\">\xE2\x96\xB6 Play</button> "
                         "<img class=\"flipbook-img\">"; // sized/hidden via the shell's .flipbook-img rule
        }
      }
    } else if (!c.enabled) {
      liveStatus = "<span class=\"badge badge-off\">DISABLED</span>";
    } else {
      liveStatus = "<span class=\"badge badge-warn\">NOT RUNNING</span> - reboot to apply";
    }

    // Notes as a tooltip so long text doesn't widen the column.
    String notesCell = c.notes.length() > 0
        ? "<span title=\"" + htmlEscape(c.notes) + "\" style=\"cursor:help;\">\xF0\x9F\x93\x9D</span>"
        : "";

    String enabledBadge = c.enabled ? "<span class=\"badge badge-on\">yes</span>"
                                      : "<span class=\"badge badge-off\">no</span>";
    html += "<tr><td>" + htmlEscape(c.name) + "</td><td>" + htmlEscape(c.deviceServiceUrl) +
            "</td><td>" + enabledBadge + "</td><td>" + liveStatus + "</td><td>" +
            lastAlertStr + "</td><td>" + previewCell + "</td><td>" +
            notesCell + "</td><td><div class=\"row-actions\">";
    // Best-guess link to the camera's own web UI on port 80. The ONVIF port is
    // dropped: several cameras run ONVIF on a separate port (e.g. Vstarcam's
    // :10080) with the UI on 80. New tab, in case it's dead.
    String hostOnly = extractHost(c.deviceServiceUrl);
    int portSep = hostOnly.indexOf(':');
    if (portSep >= 0) hostOnly = hostOnly.substring(0, portSep);
    // Must escape: deviceServiceUrl can come from a rogue camera's discovery
    // reply or an imported file - stored XSS otherwise.
    html += "<a class=\"icon-btn secondary\" href=\"http://" + htmlEscape(hostOnly) +
            "/\" target=\"_blank\" title=\"Open camera's web UI\" aria-label=\"Open camera's web UI\">"
            "&#8599;</a>";
    // RTSP link with embedded credentials, ready for VLC. Escaped: the URI
    // comes from the camera's own (untrusted) response.
    if (rtspUri.length() > 0) {
      String rtspWithCreds = buildUriWithCredentials(rtspUri, c.user, c.pass);
      html += " <a class=\"icon-btn secondary\" href=\"" + htmlEscape(rtspWithCreds) +
              "\" title=\"Open live RTSP stream (e.g. in VLC)\" aria-label=\"Open live RTSP stream\">"
              "&#9654;</a>";
    }
    // Inline MJPEG preview (rare: needs a JPEG profile with HTTP transport). A
    // toggle rather than always on: the browser holds a connection straight to
    // the camera, which can crowd out our own polling on 1-2-connection
    // cameras.
    if (mjpegUri.length() > 0) {
      String mjpegWithCreds = buildUriWithCredentials(mjpegUri, c.user, c.pass);
      String liveRowId = "liverow" + String((unsigned)rowIdx);
      String liveImgId = "liveimg" + String((unsigned)rowIdx);
      html += " <button type=\"button\" class=\"icon-btn secondary\" title=\"Toggle live preview - "
              "holds an extra connection open on the camera while showing\" aria-label=\"Toggle live "
              "preview\" onclick=\"toggleLivePreview(this,'" + liveRowId + "','" + liveImgId + "','" +
              htmlEscape(mjpegWithCreds) + "')\">\xE2\x96\xB6 Live</button>";
      liveRowHtml = "<tr id=\"" + liveRowId + "\" style=\"display:none;\"><td colspan=\"8\">"
                    "<img id=\"" + liveImgId + "\" style=\"max-width:100%;max-height:400px;\" "
                    "alt=\"live preview\"></td></tr>";
    }
    if (isLive) {
      // POST form: fetches a snapshot and sends it via a background task. The
      // kind selector picks the caption variant so phone automations can be
      // tested (pet is a separate flag, as in real alerts).
      html += " <form method=\"POST\" action=\"/cameras/test-alert\" style=\"display:inline;\">"
              "<input type=\"hidden\" name=\"name\" value=\"" + htmlEscape(c.name) + "\">"
              "<select name=\"kind\" title=\"Which detection kind to simulate\">"
              "<option value=\"generic\">Generic</option>"
              "<option value=\"person\">Person</option>"
              "<option value=\"vehicle\">Vehicle</option>"
              "<option value=\"pet\">Pet</option>"
              "</select>"
              "<button type=\"submit\" class=\"icon-btn secondary\" title=\"Send test alert\" "
              "aria-label=\"Send test alert\">\xF0\x9F\xA7\xAA</button></form>";
    }
    html += renderEditDeleteActions("/cameras/edit?name=", "/delete", c.name) + "</div></td></tr>";
    html += liveRowHtml;
    rowIdx++;
  }
  html += "</table>";

  // playFlipbook: cycles a row's cached thumbnails, oldest to newest, in a
  // larger image. Click again to stop.
  html += "<script>"
          "function playFlipbook(btn,urlList){"
          "var urls=urlList.split('|');"
          "var img=btn.nextElementSibling;"
          "if(btn.dataset.timer){clearInterval(Number(btn.dataset.timer));delete btn.dataset.timer;"
          "btn.textContent='\xE2\x96\xB6 Play';return;}"
          "btn.textContent='\xE2\x8F\xB8 Stop';img.style.display='inline';"
          "var i=0;img.src=urls[0];"
          "var timer=setInterval(function(){i=(i+1)%urls.length;img.src=urls[i];},400);"
          "btn.dataset.timer=String(timer);"
          "}"
          // toggleLivePreview: shows the preview row and sets the img src;
          // clearing src releases the camera connection.
          "function toggleLivePreview(btn,rowId,imgId,src){"
          "var row=document.getElementById(rowId);"
          "var img=document.getElementById(imgId);"
          "if(row.style.display==='table-row'){"
          "row.style.display='none';img.src='';btn.textContent='\xE2\x96\xB6 Live';return;}"
          "row.style.display='table-row';img.src=src;btn.textContent='\xE2\x8F\xB8 Stop';"
          "}"
          "</script>";

  if (!cams.empty()) {
    html += "<fieldset><legend>Mute / Unmute all</legend>";
    html += "<form method=\"POST\" action=\"/cameras/mute-all\" "
            "onsubmit=\"return confirm('Mute every enabled camera\\'s alerts? Detection/recording keeps "
            "running either way, only the Telegram send is muted.');\">";
    html += "<label>Mute every enabled camera's alerts for (minutes, or HH:MM, blank = until "
            "manually unmuted)<input type=\"text\" name=\"duration\" placeholder=\"e.g. 30 or 23:00\">"
            "</label>";
    html += "<p><button type=\"submit\">Mute all</button></p></form>";
    html += "<form method=\"POST\" action=\"/cameras/unmute-all\" "
            "onsubmit=\"return confirm('Unmute every camera\\'s alerts?');\">"
            "<p><button type=\"submit\">Unmute all</button></p></form>";
    html += "<p class=\"hint\">Same effect as Telegram's /off all and /on all - each enabled camera "
            "keeps its own subscription and detection running either way, only the Telegram send is "
            "muted. A per-camera edit or timer below still overrides whatever this last set.</p>";
    html += "</fieldset>";

    html += "<fieldset><legend>Set quiet hours for all cameras</legend>";
    html += "<form method=\"POST\" action=\"/cameras/quiet-hours-all\" "
            "onsubmit=\"return confirm('Overwrite every camera\\'s individual quiet hours setting "
            "with this one? There is no way to see what each camera currently has before this "
            "replaces it.');\">";
    html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"quietHoursEnabled\"> Quiet hours "
            "(mutes motion alerts only - tamper/offline still alert)</label>";
    html += "<label>Start<input type=\"time\" name=\"quietStart\" value=\"00:00\"></label>";
    html += "<label>End<input type=\"time\" name=\"quietEnd\" value=\"00:00\"></label>";
    html += "<p><button type=\"submit\">Apply to all cameras</button></p></form>";
    html += "<p class=\"hint\">Overwrites every camera's quiet hours (both enabled and disabled ones) "
            "with this one setting, all at once - there's no per-camera preview here, so check each "
            "camera's own Edit form afterward if you need to confirm what landed. Leaving start and "
            "end the same (e.g. both 00:00) means no active window, same as the per-camera form.</p>";
    html += "</fieldset>";

    // Each bulk checkbox starts checked only if every camera has it on, so
    // submitting untouched changes nothing; "X of Y" shows mixed states.
    size_t personOnCount = 0, vehicleOnCount = 0, petOnCount = 0;
    for (auto& c : cams) {
      if (c.personAlertsEnabled) personOnCount++;
      if (c.vehicleAlertsEnabled) vehicleOnCount++;
      if (c.petAlertsEnabled) petOnCount++;
    }
    html += "<fieldset><legend>Set person/vehicle/pet alerts for all cameras</legend>";
    html += "<form method=\"POST\" action=\"/cameras/person-alerts-all\" "
            "onsubmit=\"return confirm('Overwrite every camera\\'s individual PERSON alert setting with "
            "this one? Vehicle and pet alert settings are left untouched. There is no way to see what "
            "each camera currently has before this replaces it.');\">";
    html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
            String(personOnCount == cams.size() ? " checked" : "") + "> "
            "Alert on person detection</label>";
    html += "<p class=\"hint\">Currently on for " + String((unsigned)personOnCount) + " of " +
            String((unsigned)cams.size()) + " camera(s).</p>";
    html += "<p><button type=\"submit\">Apply person alerts to all cameras</button></p></form>";
    html += "<form method=\"POST\" action=\"/cameras/vehicle-alerts-all\" "
            "onsubmit=\"return confirm('Overwrite every camera\\'s individual VEHICLE alert setting with "
            "this one? Person and pet alert settings are left untouched. There is no way to see what "
            "each camera currently has before this replaces it.');\">";
    html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
            String(vehicleOnCount == cams.size() ? " checked" : "") + "> "
            "Alert on vehicle detection</label>";
    html += "<p class=\"hint\">Currently on for " + String((unsigned)vehicleOnCount) + " of " +
            String((unsigned)cams.size()) + " camera(s).</p>";
    html += "<p><button type=\"submit\">Apply vehicle alerts to all cameras</button></p></form>";
    html += "<form method=\"POST\" action=\"/cameras/pet-alerts-all\" "
            "onsubmit=\"return confirm('Overwrite every camera\\'s individual PET alert setting with this "
            "one? Person and vehicle alert settings are left untouched. There is no way to see what each "
            "camera currently has before this replaces it.');\">";
    html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
            String(petOnCount == cams.size() ? " checked" : "") + "> "
            "Alert on pet (dog/cat) detection</label>";
    html += "<p class=\"hint\">Currently on for " + String((unsigned)petOnCount) + " of " +
            String((unsigned)cams.size()) + " camera(s).</p>";
    html += "<p><button type=\"submit\">Apply pet alerts to all cameras</button></p></form>";
    html += "<p class=\"hint\">Each button only overwrites its own setting (person, vehicle, or pet) "
            "across every camera (both enabled and disabled ones) - the other two stay whatever each "
            "camera already has, so bulk-setting one never undoes a deliberate per-camera choice on "
            "another. The \"currently on for X of Y\" counts (and each checkbox's own starting state) "
            "reflect the real setting right now, but only as an all-on/not-all-on summary - if it's "
            "mixed, check each camera's own Edit form for the specific breakdown. Person/vehicle only "
            "affects cameras whose own ONVIF AI actually distinguishes that detection - a plain motion "
            "sensor always alerts regardless.</p>";
    html += "</fieldset>";
  }

  html += "<form method=\"POST\" action=\"/cameras/discover\">"
          "<p><button type=\"submit\">Search network for cameras</button></p></form>";
  html += "<p class=\"hint\">Sends a WS-Discovery probe on the local network segment and lists what "
          "answers, like an NVR's own camera search - click Add next to a result to prefill the Add "
          "form below with its address (WS-Discovery never carries credentials, so username/password "
          "still need to be typed in by hand). Runs in the background; reload this page after clicking "
          "to see results. Multicast discovery only reaches devices on the same network segment as "
          "this board, so a camera on a different VLAN/subnet won't show up here even if it's directly "
          "reachable by URL.</p>";
  html += renderCameraDiscoveryStatus();

  if (!cams.empty()) {
    html += "<form method=\"POST\" action=\"/cameras/test-all\">"
            "<p><button type=\"submit\">Test all cameras</button></p></form>";
    html += "<p class=\"hint\">Checks reachability and ONVIF event-service response (not a full "
            "subscription test - see below) for every already-saved ENABLED camera at once, not "
            "whatever's currently typed into the form below - useful after a network change to see "
            "which cameras, if any, broke. Runs in the background, so the dashboard stays responsive "
            "to everyone else while it works - reload this page after clicking to see results once "
            "ready. Deliberately doesn't create a test subscription the way the single-camera Test "
            "Connection button does, since most enabled cameras already have a real one from their own "
            "monitoring task, and creating a second one on every camera at once is a bigger risk than "
            "doing it for one camera you're actively editing.</p>";
    html += renderTestAllStatus();
  }

  html += renderTestConnectionStatus();
  html += renderTestAlertStatus();
  html += renderCameraForm(prefill ? *prefill : CameraConfig(), isEdit);

  html += "<p class=\"hint\">Adding, editing, or deleting a camera updates storage immediately, "
          "but only takes effect after the board reboots. Test Connection doesn't save anything - "
          "it just runs GetCapabilities/GetEventProperties/GetSnapshotUri against whatever is "
          "currently typed in, so you can catch a wrong URL or credential before rebooting. Runs "
          "in the background; reload this page after clicking to see the result.</p>";
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
  // Blank/zero falls back to the default (0 would alert every poll). Capped at
  // 24h: seconds * 1000 overflows 32 bits. Re-clamped at use too.
  long cooldownMaxSec = (long)(CAMERA_ALERT_COOLDOWN_MAX_MS / 1000UL);
  if (cooldownSec > cooldownMaxSec) cooldownSec = cooldownMaxSec;
  c.alertCooldownMs = cooldownSec > 0 ? (unsigned long)cooldownSec * 1000UL : CameraConfig().alertCooldownMs;

  long offlineMin = request->getParam("offlineThresholdMin", "5").toInt();
  // Capped for the same overflow (minutes * 60000).
  long offlineMaxMin = (long)(CAMERA_OFFLINE_THRESHOLD_MAX_MS / 60000UL);
  if (offlineMin > offlineMaxMin) offlineMin = offlineMaxMin;
  c.offlineThresholdMs = offlineMin > 0 ? (unsigned long)offlineMin * 60000UL : CameraConfig().offlineThresholdMs;

  long burstCount = request->getParam("snapshotBurstCount", "1").toInt();
  // 1..CAMERA_SNAPSHOT_BURST_MAX, so a typo can't flood Telegram.
  if (burstCount < 1) burstCount = 1;
  if (burstCount > (long)CAMERA_SNAPSHOT_BURST_MAX) burstCount = (long)CAMERA_SNAPSHOT_BURST_MAX;
  c.snapshotBurstCount = (unsigned int)burstCount;

  c.quietHoursEnabled = request->hasParam("quietHoursEnabled");
  c.quietStartMinute  = parseHHMMToMinutes(request->getParam("quietStart", "00:00"));
  c.quietEndMinute    = parseHHMMToMinutes(request->getParam("quietEnd", "00:00"));

  // 0 means off here, so only clamp negatives and the ceiling.
  long watchdogHours = request->getParam("motionWatchdogHours", "0").toInt();
  if (watchdogHours < 0) watchdogHours = 0;
  if (watchdogHours > 168) watchdogHours = 168; // 1 week
  c.motionWatchdogHours = (uint16_t)watchdogHours;

  long timelapseMin = request->getParam("timelapseIntervalMin", "0").toInt();
  if (timelapseMin < 0) timelapseMin = 0;
  if (timelapseMin > 1440) timelapseMin = 1440; // 24h
  c.timelapseIntervalMin = (uint16_t)timelapseMin;
  c.timelapseSendToTelegram = request->hasParam("timelapseSendToTelegram");

  // 0 means "use the global setting".
  long retentionDays = request->getParam("retentionDays", "0").toInt();
  if (retentionDays < 0) retentionDays = 0;
  if (retentionDays > (long)SD_RETENTION_MAX_DAYS) retentionDays = (long)SD_RETENTION_MAX_DAYS;
  c.retentionDays = (uint16_t)retentionDays;

  c.personAlertsEnabled = request->hasParam("personAlertsEnabled");
  c.vehicleAlertsEnabled = request->hasParam("vehicleAlertsEnabled");
  c.petAlertsEnabled = request->hasParam("petAlertsEnabled");
  c.petAlertsTextOnly = request->hasParam("petAlertsTextOnly");
  c.motionDigestEnabled = request->hasParam("motionDigestEnabled");

  // 0 means unset (no substitution).
  long snapshotMaxWidth = request->getParam("snapshotMaxWidth", "0").toInt();
  if (snapshotMaxWidth < 0) snapshotMaxWidth = 0;
  if (snapshotMaxWidth > (long)CAMERA_SNAPSHOT_DIMENSION_MAX) snapshotMaxWidth = (long)CAMERA_SNAPSHOT_DIMENSION_MAX;
  c.snapshotMaxWidth = (uint16_t)snapshotMaxWidth;

  long snapshotMaxHeight = request->getParam("snapshotMaxHeight", "0").toInt();
  if (snapshotMaxHeight < 0) snapshotMaxHeight = 0;
  if (snapshotMaxHeight > (long)CAMERA_SNAPSHOT_DIMENSION_MAX) snapshotMaxHeight = (long)CAMERA_SNAPSHOT_DIMENSION_MAX;
  c.snapshotMaxHeight = (uint16_t)snapshotMaxHeight;

  // Blank/zero falls back to the default; a too-low value is raised to the
  // floor rather than discarded. Re-clamped at use too.
  long pollIntervalMs = request->getParam("pollIntervalMs", "2000").toInt();
  if (pollIntervalMs <= 0) pollIntervalMs = (long)CameraConfig().pollIntervalMs;
  if (pollIntervalMs < (long)CAMERA_POLL_INTERVAL_MIN_MS) pollIntervalMs = (long)CAMERA_POLL_INTERVAL_MIN_MS;
  if (pollIntervalMs > (long)CAMERA_POLL_INTERVAL_MAX_MS) pollIntervalMs = (long)CAMERA_POLL_INTERVAL_MAX_MS;
  c.pollIntervalMs = (unsigned long)pollIntervalMs;

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
    // New camera: staged for loop() to add live, unless MAX_CAMERAS is used up
    // (then a reboot is needed, and the note says so).
    if (stagePendingNewCamera(cam)) {
      applyNote = cam.enabled
          ? ("\"" + cam.name + "\" added - starting monitoring now (no reboot needed).")
          : ("\"" + cam.name + "\" added (disabled) - no reboot needed; enable it whenever you're ready.");
    } else {
      applyNote = "\"" + cam.name + "\" added, but this board's reserved camera capacity is already used "
                  "up - reboot to bring it online.";
    }
    return true;
  }

  if (cam.pass.length() == 0) {
    for (auto& existing : loadCameras()) {
      if (existing.name.equalsIgnoreCase(originalName)) { cam.pass = existing.pass; break; }
    }
  }

  // The rest runs under g_saveMutex (see above). It also serializes the
  // store's read-modify-write for saves of different cameras.
  xSemaphoreTake(g_saveMutex, portMAX_DELAY);

  // By original name (a rename keeps the slot). A live task exists exactly
  // when the slot's in-memory enabled is true.
  int idx = findLiveCameraIndex(liveCameras, originalName);
  bool wasRunning = (idx >= 0) && liveStates && idx < (int)liveStates->size() && (*liveCameras)[idx].enabled;

  if (!updateCamera(originalName, cam)) {
    xSemaphoreGive(g_saveMutex);
    banner = "Could not save \"" + htmlEscape(cam.name) +
             "\" - a different camera already uses that name.";
    return false;
  }

  if (idx >= 0 && liveStates && idx < (int)liveStates->size()) {
    // Not escaped here: applyNote travels in the redirect and is escaped once,
    // where it's rendered. Escaping twice once let a direct ?note= request
    // skip escaping entirely (reflected XSS).
    if (wasRunning && cam.enabled) {
      // Stage the config for the running task; this task must never write its
      // CameraConfig directly (see requestLiveConfigReload).
      requestLiveConfigReload((*liveStates)[idx], cam);
      applyNote = "\"" + cam.name + "\" updated - applying live, reconnecting now (no reboot needed).";
    } else if (!wasRunning && cam.enabled) {
      // Newly enabled: stage the config via pendingConfig (other tasks read
      // enabled/name unlocked, so don't write the slot directly) and create
      // the lock first.
      cameraStateInit((*liveStates)[idx]);
      requestLiveConfigReload((*liveStates)[idx], cam);
      spawnCameraTask(idx); // its own startup applies the staged config - see applyPendingConfigIfAny
      logEvent(cam.name + ": enabled via dashboard, monitoring started live");
      applyNote = "\"" + cam.name + "\" enabled - monitoring started live (no reboot needed).";
    } else if (wasRunning && !cam.enabled) {
      // A disable wins over any other change in the same save.
      requestCameraStop((*liveStates)[idx]);
      applyNote = "\"" + cam.name + "\" disabled - monitoring stopped live (no reboot needed).";
    }
  }
  // idx < 0: no live slot (added while capacity was full) - needs a reboot.

  xSemaphoreGive(g_saveMutex);
  return true;
}

bool stopLiveCameraIfRunning(const String& name, std::vector<CameraConfig>* liveCameras,
                              std::vector<CameraState>* liveStates) {
  int idx = findLiveCameraIndex(liveCameras, name);
  bool running = (idx >= 0) && liveStates && idx < (int)liveStates->size() && (*liveCameras)[idx].enabled;
  if (running) requestCameraStop((*liveStates)[idx]);
  return running;
}

String applyQuietHoursToAllCameras(PsychicRequest* request, std::vector<CameraConfig>* liveCameras,
                                    std::vector<CameraState>* liveStates) {
  bool quietHoursEnabled = request->hasParam("quietHoursEnabled");
  uint16_t quietStartMinute = parseHHMMToMinutes(request->getParam("quietStart", "00:00"));
  uint16_t quietEndMinute = parseHHMMToMinutes(request->getParam("quietEnd", "00:00"));

  // Every camera, including disabled ones, so the setting sticks if enabled
  // later. updateAllCameras reads and writes in one critical section.
  size_t cameraCount = loadCameras().size();
  if (cameraCount == 0) return "No cameras configured - nothing to apply this to.";

  bool saved = updateAllCameras([&](CameraConfig& c) {
    c.quietHoursEnabled = quietHoursEnabled;
    c.quietStartMinute = quietStartMinute;
    c.quietEndMinute = quietEndMinute;
  });
  if (!saved) {
    return "Failed to save - NVS write error (see Serial log). Quiet hours were NOT changed for any camera.";
  }

  // Live-reload each running enabled camera, like a single edit. Re-reads the
  // list, which the write above has committed.
  std::vector<CameraConfig> cams = loadCameras();
  size_t liveReloaded = 0;
  if (liveCameras && liveStates) {
    for (auto& c : cams) {
      int idx = findLiveCameraIndex(liveCameras, c.name);
      bool wasRunning = idx >= 0 && idx < (int)liveStates->size() && (*liveCameras)[idx].enabled;
      if (wasRunning) {
        requestLiveConfigReload((*liveStates)[idx], c);
        liveReloaded++;
      }
    }
  }

  String result = "Quiet hours " +
      (quietHoursEnabled ? ("enabled, " + minutesToHHMM(quietStartMinute) + "-" + minutesToHHMM(quietEndMinute))
                          : String("disabled")) +
      " - applied to all " + String(cams.size()) + " camera(s).";
  if (liveReloaded > 0) {
    result += " " + String(liveReloaded) + " already-running camera(s) are reconnecting now with it.";
  }
  result += " Any camera not currently running (disabled, or added after this board's last boot) "
            "needs a reboot to pick this up.";
  return result;
}

// Like applyQuietHoursToAllCameras, for personAlertsEnabled. Each alert type
// has its own button: a combined form once re-enabled a camera's
// deliberately-disabled vehicle alerts.
String applyPersonAlertsToAllCameras(PsychicRequest* request, std::vector<CameraConfig>* liveCameras,
                                      std::vector<CameraState>* liveStates) {
  bool enabled = request->hasParam("enabled");

  size_t cameraCount = loadCameras().size();
  if (cameraCount == 0) return "No cameras configured - nothing to apply this to.";

  bool saved = updateAllCameras([&](CameraConfig& c) { c.personAlertsEnabled = enabled; });
  if (!saved) {
    return "Failed to save - NVS write error (see Serial log). Person alert settings were NOT changed for "
           "any camera.";
  }

  std::vector<CameraConfig> cams = loadCameras();
  size_t liveReloaded = 0;
  if (liveCameras && liveStates) {
    for (auto& c : cams) {
      int idx = findLiveCameraIndex(liveCameras, c.name);
      bool wasRunning = idx >= 0 && idx < (int)liveStates->size() && (*liveCameras)[idx].enabled;
      if (wasRunning) {
        requestLiveConfigReload((*liveStates)[idx], c);
        liveReloaded++;
      }
    }
  }

  String result = "Person alerts " + String(enabled ? "enabled" : "disabled") + " - applied to all " +
      String(cams.size()) + " camera(s). Vehicle alert settings were left untouched.";
  if (liveReloaded > 0) {
    result += " " + String(liveReloaded) + " already-running camera(s) are reconnecting now with it.";
  }
  result += " Any camera not currently running (disabled, or added after this board's last boot) "
            "needs a reboot to pick this up.";
  return result;
}

String applyVehicleAlertsToAllCameras(PsychicRequest* request, std::vector<CameraConfig>* liveCameras,
                                       std::vector<CameraState>* liveStates) {
  bool enabled = request->hasParam("enabled");

  size_t cameraCount = loadCameras().size();
  if (cameraCount == 0) return "No cameras configured - nothing to apply this to.";

  bool saved = updateAllCameras([&](CameraConfig& c) { c.vehicleAlertsEnabled = enabled; });
  if (!saved) {
    return "Failed to save - NVS write error (see Serial log). Vehicle alert settings were NOT changed for "
           "any camera.";
  }

  std::vector<CameraConfig> cams = loadCameras();
  size_t liveReloaded = 0;
  if (liveCameras && liveStates) {
    for (auto& c : cams) {
      int idx = findLiveCameraIndex(liveCameras, c.name);
      bool wasRunning = idx >= 0 && idx < (int)liveStates->size() && (*liveCameras)[idx].enabled;
      if (wasRunning) {
        requestLiveConfigReload((*liveStates)[idx], c);
        liveReloaded++;
      }
    }
  }

  String result = "Vehicle alerts " + String(enabled ? "enabled" : "disabled") + " - applied to all " +
      String(cams.size()) + " camera(s). Person alert settings were left untouched.";
  if (liveReloaded > 0) {
    result += " " + String(liveReloaded) + " already-running camera(s) are reconnecting now with it.";
  }
  result += " Any camera not currently running (disabled, or added after this board's last boot) "
            "needs a reboot to pick this up.";
  return result;
}

String applyPetAlertsToAllCameras(PsychicRequest* request, std::vector<CameraConfig>* liveCameras,
                                   std::vector<CameraState>* liveStates) {
  bool enabled = request->hasParam("enabled");

  size_t cameraCount = loadCameras().size();
  if (cameraCount == 0) return "No cameras configured - nothing to apply this to.";

  bool saved = updateAllCameras([&](CameraConfig& c) { c.petAlertsEnabled = enabled; });
  if (!saved) {
    return "Failed to save - NVS write error (see Serial log). Pet alert settings were NOT changed for "
           "any camera.";
  }

  std::vector<CameraConfig> cams = loadCameras();
  size_t liveReloaded = 0;
  if (liveCameras && liveStates) {
    for (auto& c : cams) {
      int idx = findLiveCameraIndex(liveCameras, c.name);
      bool wasRunning = idx >= 0 && idx < (int)liveStates->size() && (*liveCameras)[idx].enabled;
      if (wasRunning) {
        requestLiveConfigReload((*liveStates)[idx], c);
        liveReloaded++;
      }
    }
  }

  String result = "Pet alerts " + String(enabled ? "enabled" : "disabled") + " - applied to all " +
      String(cams.size()) + " camera(s). Person/vehicle alert settings were left untouched.";
  if (liveReloaded > 0) {
    result += " " + String(liveReloaded) + " already-running camera(s) are reconnecting now with it.";
  }
  result += " Any camera not currently running (disabled, or added after this board's last boot) "
            "needs a reboot to pick this up.";
  return result;
}

// Creates one real test subscription, left to expire. The banner is built from
// submitted fields and rendered unescaped, so name and URL are escaped here.
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

  String topics, unusedTopics;
  if (cameraGetEventServiceCapabilities(cfg, st) && cameraGetEventProperties(cfg, st, &topics, &unusedTopics)) {
    result += " Event service responds normally.";
    // Keyword scan, the same matching live events use.
    result += topics.length() > 0
        ? (" This camera's event schema mentions: " + htmlEscape(topics) + ".")
        : (" This camera's event schema doesn't mention any detection type this project recognizes by "
           "name (PeopleDetect/VehicleDetect/DogCatDetect/MotionAlarm/CellMotionDetector/"
           "TamperDetector/SignalLoss) - it may still report plain motion under a topic name this "
           "project doesn't know to look for yet, or use a vendor-specific scheme entirely.");
    if (unusedTopics.length() > 0) {
      result += " It also advertises: " + htmlEscape(unusedTopics) + " - not used by this firmware yet.";
    }
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

// ============================================================
// Background wrapper. cfg is the unsaved form data, so it's copied into the
// task.
// ============================================================

static BackgroundJob<String> g_testConnectionJob;

static void testConnectionTask(void* param) {
  CameraConfig* cfg = static_cast<CameraConfig*>(param);
  String result = testCameraConnection(*cfg); // the actual (slow) work
  delete cfg;
  g_testConnectionJob.finish(result);
  vTaskDelete(nullptr);
}

BackgroundJobStartOutcome startTestConnectionAsync(const CameraConfig& cfg) {
  if (!g_testConnectionJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one test at a time - a second click while one's in flight is a no-op

  // Freed by the task.
  CameraConfig* cfgCopy = new CameraConfig(cfg);
  // Same stack as a monitoring task (same TLS/SOAP work).
  BaseType_t created = xTaskCreate(testConnectionTask, "testConn", 10240, cfgCopy, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // Undo tryStart(), or nothing would ever call finish().
    delete cfgCopy; // never handed to a task, so nothing else will free it
    g_testConnectionJob.cancelStart();
    Serial.println("[webserver_cameras] ERROR: failed to start the Test Connection task (out of memory?) "
                    "- try again once memory frees up.");
    return BackgroundJobStartOutcome::FailedToStart;
  }
  return BackgroundJobStartOutcome::Started;
}

String renderTestConnectionStatus() {
  auto st = g_testConnectionJob.status();
  if (st.inProgress) {
    return "<p class=\"hint\">Testing camera connection in the background - reload this page in a "
           "moment to see the result. The rest of the dashboard stays responsive to everyone else "
           "in the meantime.</p>";
  }
  if (st.hasResult) return "<p>" + st.result + "</p>";
  return "";
}

// ============================================================
// Send Test Alert background wrapper.
// ============================================================

struct TestAlertResult {
  bool ok = false;
  String cameraName;
  String detail; // failure reason - "" on success
};

static BackgroundJob<TestAlertResult> g_testAlertJob;

struct TestAlertTaskParams {
  CameraConfig cfg; // heap-copied snapshot - only cfg.name is actually read by sendTestAlert
  CameraState* st;  // NOT owned - must be the live CameraState (liveStates[idx]), see startTestAlertAsync's comment
  MotionDetectionKind kind;
  bool isPetEvent;
};

static void testAlertTask(void* param) {
  TestAlertTaskParams* p = static_cast<TestAlertTaskParams*>(param);
  TestAlertResult r;
  r.cameraName = p->cfg.name;
  r.ok = sendTestAlert(p->cfg, *p->st, r.detail, p->kind, p->isPetEvent);
  delete p;
  g_testAlertJob.finish(r);
  vTaskDelete(nullptr);
}

BackgroundJobStartOutcome startTestAlertAsync(const CameraConfig& cfg, CameraState& st, MotionDetectionKind kind,
                                               bool isPetEvent) {
  if (!g_testAlertJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one at a time - a second click while one's in flight is a no-op

  TestAlertTaskParams* params = new TestAlertTaskParams{cfg, &st, kind, isPetEvent};
  // Same stack as the other camera tasks.
  BaseType_t created = xTaskCreate(testAlertTask, "testAlert", 10240, params, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    delete params; // never handed to a task, so nothing else will free it
    g_testAlertJob.cancelStart();
    Serial.println("[webserver_cameras] ERROR: failed to start the Send Test Alert task (out of memory?) "
                    "- try again once memory frees up.");
    return BackgroundJobStartOutcome::FailedToStart;
  }
  return BackgroundJobStartOutcome::Started;
}

String renderTestAlertStatus() {
  auto st = g_testAlertJob.status();
  if (st.inProgress) {
    return "<p class=\"hint\">Sending test alert in the background - reload this page in a moment to "
           "see the result. The rest of the dashboard stays responsive to everyone else in the "
           "meantime.</p>";
  }
  if (!st.hasResult) return "";
  if (st.result.ok) {
    return "<p>Test alert sent for \"" + htmlEscape(st.result.cameraName) + "\".</p>";
  }
  return "<p>Test alert FAILED for \"" + htmlEscape(st.result.cameraName) + "\": " +
         htmlEscape(st.result.detail) + "</p>";
}

// Read-only subset of testCameraConnection for the Test-all table. No test
// subscription: every monitored camera already holds one, and some firmware
// allows only one.
static CameraTestResult testOneCameraConnectionBrief(const CameraConfig& cfg) {
  CameraTestResult r;
  r.name = cfg.name;

  CameraState st;
  if (!resolveCameraCredentials(cfg, st)) {
    r.detail = "no username/password set";
    return r;
  }

  if (!cameraDiscoverServices(cfg, st)) {
    r.detail = "device service unreachable, or no ONVIF event service found";
    return r;
  }
  r.reachable = true;

  if (cameraGetEventServiceCapabilities(cfg, st) && cameraGetEventProperties(cfg, st)) {
    r.eventServiceOk = true;
  } else {
    r.detail = "event service didn't respond to GetServiceCapabilities/GetEventProperties";
  }

  return r;
}

// Every enabled camera in NVS, one after another; can take minutes, hence the
// background task.
std::vector<CameraTestResult> testAllCameraConnections() {
  std::vector<CameraTestResult> results;
  for (auto& cfg : loadCameras()) {
    if (!cfg.enabled) {
      CameraTestResult r;
      r.name = cfg.name;
      r.skipped = true;
      results.push_back(r);
      continue;
    }
    results.push_back(testOneCameraConnectionBrief(cfg));
  }
  return results;
}

// ============================================================
// Test-all background wrapper.
// ============================================================

static BackgroundJob<std::vector<CameraTestResult>> g_testAllJob;

static void testAllCamerasTask(void*) {
  g_testAllJob.finish(testAllCameraConnections()); // the actual (slow) work
  vTaskDelete(nullptr);
}

BackgroundJobStartOutcome startTestAllCamerasAsync() {
  // Deferred while a Telegram send is in flight: overlapping it with a SOAP
  // burst has driven the heap dangerously low. Reported as AlreadyRunning
  // ("try again").
  if (telegramSendInProgress()) return BackgroundJobStartOutcome::AlreadyRunning;
  if (!g_testAllJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one run at a time - a second click while one's in flight is a no-op

  // Same stack as a monitoring task.
  BaseType_t created = xTaskCreate(testAllCamerasTask, "testAllCams", 10240, nullptr, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // Undo tryStart() (see startTestConnectionAsync).
    g_testAllJob.cancelStart();
    Serial.println("[webserver_cameras] ERROR: failed to start the Test All Cameras task (out of memory?) "
                    "- try again once memory frees up.");
    return BackgroundJobStartOutcome::FailedToStart;
  }
  return BackgroundJobStartOutcome::Started;
}

String renderTestAllStatus() {
  auto st = g_testAllJob.status();
  if (st.inProgress) {
    return "<p class=\"hint\">A camera connectivity test is running in the background - reload this "
           "page in a bit to see results. The rest of the dashboard stays responsive to everyone else "
           "in the meantime.</p>";
  }
  if (st.hasResult) return renderCameraTestAllResults(st.result);
  return "";
}

String renderCameraTestAllResults(const std::vector<CameraTestResult>& results) {
  if (results.empty()) {
    return "No cameras configured yet - nothing to test.";
  }
  std::vector<std::vector<String>> rows;
  for (auto& r : results) {
    if (r.skipped) {
      rows.push_back({r.name, "Skipped (disabled)", "", ""});
      continue;
    }
    rows.push_back({r.name, r.reachable ? "OK" : "FAIL", r.eventServiceOk ? "OK" : "FAIL", r.detail});
  }
  return "<p>Test all cameras - results:</p>" +
         renderDataTable({"Camera", "Reachable", "Event service", "Detail"}, rows);
}

// ============================================================
// Network camera discovery (WS-Discovery).
// ============================================================

static const IPAddress kWsDiscoveryMulticastAddr(239, 255, 255, 250);
static const uint16_t  kWsDiscoveryPort = 3702;

// Listen window. Widened from 4s after discovery found 2 of several cameras
// that a PC tool found; some cheap stacks answer slowly (or not at all while
// serving a subscription). The page polls every 2s regardless.
static const unsigned long kDiscoveryListenMs = 12000;

// Multicast over busy WiFi gets dropped, so re-probe about once a second
// across the window.
static const int kDiscoveryProbeCount = 8;

static BackgroundJob<std::vector<DiscoveredCamera>> g_discoveryJob;

// Blocks for the whole listen window; runs on the discovery task.
static std::vector<DiscoveredCamera> runCameraDiscovery() {
  std::vector<DiscoveredCamera> found;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[webserver_cameras] Camera discovery: WiFi not connected - skipping.");
    return found;
  }

  WiFiUDP udp;
  if (udp.begin(0) == 0) {
    Serial.println("[webserver_cameras] Camera discovery: udp.begin() failed - skipping.");
    return found; // 0 = OS-assigned ephemeral local port
  }

  Serial.println("[webserver_cameras] Camera discovery: listening for WS-Discovery replies...");
  unsigned long start = millis();
  unsigned long nextProbeMs = start;
  int probesSent = 0;
  char buf[2048];

  while ((long)(millis() - start) < (long)kDiscoveryListenMs) {
    if (probesSent < kDiscoveryProbeCount && (long)(millis() - nextProbeMs) >= 0) {
      String probe = buildProbeMessage(makeUUID());
      udp.beginPacket(kWsDiscoveryMulticastAddr, kWsDiscoveryPort);
      udp.write((const uint8_t*)probe.c_str(), probe.length());
      udp.endPacket();
      probesSent++;
      nextProbeMs = millis() + 1000;
    }

    int size = udp.parsePacket();
    if (size <= 0) {
      delay(20);
      continue;
    }
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len <= 0) continue;
    buf[len] = '\0';

    DiscoveredCamera dc;
    if (!parseProbeMatch(String(buf), dc)) continue;

    bool alreadySeen = false;
    for (auto& existing : found) {
      if (existing.xaddr == dc.xaddr) { alreadySeen = true; break; }
    }
    if (!alreadySeen) found.push_back(dc);
  }

  udp.stop();
  Serial.printf("[webserver_cameras] Camera discovery: finished, %u camera(s) found.\n", (unsigned)found.size());
  return found;
}

static void cameraDiscoveryTask(void*) {
  g_discoveryJob.finish(runCameraDiscovery());
  Serial.println("[webserver_cameras] Camera discovery: background job marked finished.");
  vTaskDelete(nullptr);
}

BackgroundJobStartOutcome startCameraDiscoveryAsync() {
  // Deferred while a Telegram send is in flight: discovery's UDP window
  // overlapping a photo send once nearly exhausted the heap.
  if (telegramSendInProgress()) return BackgroundJobStartOutcome::AlreadyRunning;
  if (!g_discoveryJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one search at a time - a second click while one's in flight is a no-op

  // No TLS, so smaller than 10240, but the 2KB receive buffer needs more than
  // 4096.
  BaseType_t created = xTaskCreate(cameraDiscoveryTask, "camDiscover", 6144, nullptr, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // Undo tryStart() (see startTestConnectionAsync).
    g_discoveryJob.cancelStart();
    Serial.println("[webserver_cameras] ERROR: failed to start the camera discovery task (out of memory?) "
                    "- try again once memory frees up.");
    return BackgroundJobStartOutcome::FailedToStart;
  }
  return BackgroundJobStartOutcome::Started;
}

String renderCameraDiscoveryStatus() {
  auto st = g_discoveryJob.status();
  if (st.inProgress) {
    return "<p class=\"hint\">Searching the network for cameras - reload this page in a few seconds "
           "to see results. The rest of the dashboard stays responsive to everyone else in the "
           "meantime.</p>";
  }
  if (!st.hasResult) return "";
  const std::vector<DiscoveredCamera>& results = st.result;
  if (results.empty()) {
    return "<p class=\"hint\">No cameras answered the search. A camera already added above won't "
           "necessarily show up again here even though it's working fine - some stacks stop announcing "
           "themselves once they have an active subscription. Cameras on a different VLAN/subnet from "
           "this board, or ones that don't support WS-Discovery at all, also won't be found this way - "
           "add those manually with their known address instead.</p>";
  }

  std::vector<DiscoveryResultRow> rows;
  for (auto& d : results) {
    String nameForForm = d.nameHint.length() > 0 ? d.nameHint : d.xaddr;
    rows.push_back({{d.xaddr, d.nameHint.length() > 0 ? d.nameHint : "(none)"},
                     {{"prefillName", nameForForm}, {"prefillUrl", d.xaddr}}});
  }
  return "<p>Cameras found on the network:</p>" +
         renderDiscoveryResultsTable({"Address", "Name hint"}, "/cameras", rows);
}

bool cameraJobsInProgress() {
  return g_testConnectionJob.status().inProgress || g_testAllJob.status().inProgress ||
         g_discoveryJob.status().inProgress || g_testAlertJob.status().inProgress;
}
