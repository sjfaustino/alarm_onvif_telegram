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

// Serializes saveCameraSubmission's "decide whether this camera was
// already running, then apply live or note a reboot's needed" section -
// without it, two near-simultaneous saves of the same newly-enabled
// camera could both observe wasRunning==false (liveCameras[i].enabled
// only flips once the spawned task's applyPendingConfigIfAny actually
// runs, asynchronously) and both spawn a task for the same slot. Saves
// are low-frequency and admin-driven, so serializing all of them (even
// different cameras) costs nothing worth a per-camera locking scheme.
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

// Embeds user:pass as scheme://user:pass@host/... userinfo - shared by
// the RTSP live-view link (rtsp://) and the MJPEG live-preview <img> src
// (http://) below, so either is directly usable without the viewer (VLC,
// an NVR, or the browser's own <img> fetch, none of which have anywhere
// else to be told a password) needing to already know the camera's
// credentials - the same ones cfg.user/cfg.pass already store in NVS, not
// a new exposure of anything. urlEncode()d (not raw) so a ':'/'@' in
// either one can't break out of the userinfo section and corrupt the
// rest of the URI. A no-op (returns rawUri unchanged) if there's no
// username to embed, or the URI already carries its own userinfo (some
// cameras' GetStreamUri response embeds one directly) - never overwrite
// credentials the camera itself already chose to include.
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

// One-glance aggregate health summary shown above the per-camera table -
// answers "is everything OK?" without scanning every row, most useful
// once there are more than a handful of cameras configured. A separate,
// lightweight pass over cams/liveStates rather than folded into the much
// longer per-row loop below - keeps both independently simple to read and
// reorder. Categories mirror that loop's own status classification
// exactly (offline/subscribed/not-live), so the counts always agree with
// what the table itself shows below them. "" (nothing rendered) if there
// are no cameras configured at all - an empty summary bar above an empty
// table would just be noise.
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
    // Same three-state classification as the per-row badge below (OFFLINE/
    // ONLINE/NOT SUBSCRIBED) - see that code's own comment for why these
    // are genuinely different situations, not degrees of the same one.
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
                      // own idx isn't usable for that (can be -1 for a camera not in liveCameras yet)
  for (auto& c : cams) {
    int idx = findLiveCameraIndex(liveCameras, c.name);
    String liveStatus;
    String lastAlertStr = "never";
    String previewCell = "<span class=\"hint\">(none yet)</span>";
    String rtspUri;  // populated below only if isLive - see CameraState::streamUri's own comment
    String mjpegUri; // populated below only if isLive - see CameraState::mjpegUri's own comment
    String liveRowHtml; // set below only if mjpegUri is non-empty - a sibling <tr> appended after this row's own
    // c.enabled (fresh from NVS, not the live cfg) is required here, not
    // just idx>=0 - a torn-down camera (requestCameraStop, disabled/deleted
    // live) keeps its liveCameras/liveStates slot forever (never shrinks),
    // so idx alone can't tell "task exists" from "task existed once, has
    // since exited". c.enabled flips to false the moment the save/delete
    // that triggered the teardown lands in NVS, well before this render
    // could see anything else. Also gates the RTSP link and Send-Test-Alert
    // button below - both need a real, live CameraState to read/act on.
    bool isLive = idx >= 0 && liveStates && idx < (int)liveStates->size() && c.enabled;
    if (isLive) {
      // Read under lock - these fields are written by the camera's own
      // task, this render runs on PsychicHttp's task. See
      // CameraState::stateMutex.
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
      // Three-state badge, not two - OFFLINE (red, hard failure - not
      // answering at all) and "not subscribed" (amber - answering fine but
      // can't hold a subscription, exactly the case checkSubscriptionHealth,
      // telegram.cpp, exists to catch) are genuinely different situations,
      // not degrees of the same one. ONLINE (green) only once both are
      // clear.
      if (offline) {
        liveStatus = "<span class=\"badge badge-offline\">OFFLINE</span>";
      } else if (subscribed) {
        liveStatus = "<span class=\"badge badge-on\">ONLINE</span>";
      } else {
        liveStatus = "<span class=\"badge badge-warn\">NOT SUBSCRIBED</span>";
      }
      // Shown regardless of whether it's currently back online - a camera
      // that's flapped offline/online repeatedly and happens to be online
      // again right now is exactly the case a live-status snapshot alone
      // would otherwise hide, same reasoning as recentReconnects below but
      // for actual OFFLINE transitions (crossed offlineThresholdMs), a
      // related but distinct signal from reconnect attempts.
      if (recentOfflineEvents > 0) {
        liveStatus += " - went offline " + String((unsigned)recentOfflineEvents) +
                      (recentOfflineEvents == EVENT_HISTORY_RING_SIZE ? "+" : "") + " time(s) in the last 24h";
      }
      if (!alertsEnabled) liveStatus += " <span class=\"badge badge-off\">MUTED</span>";
      // (long) cast for the same overflow-safe "is this due yet" check
      // used everywhere else a millis() due-timestamp is compared - see
      // CameraState::scheduledRevertDueMs's comment.
      if (revertDueMs != 0 && (long)(millis() - revertDueMs) < 0) {
        liveStatus += " - auto " + String(revertToOn ? "ON" : "OFF") + " in " +
                      formatUptime(revertDueMs - millis());
      }
      // Omitted entirely at 0 (the common, healthy case) - see
      // CameraState::totalReconnects' own comment for why this is worth
      // surfacing separately from "subscribed"/"OFFLINE" above.
      if (totalReconnects > 0) {
        liveStatus += " - " + String(totalReconnects) + " reconnect(s) since boot";
        // Distinguishes "flaky once, ages ago" from "flapping right now" -
        // see CameraState::reconnectHistory's own comment. The "+" signals
        // a floor, not an exact count: every ring slot being within 24h
        // means older reconnects may have been evicted before they could
        // be counted.
        if (recentReconnects > 0) {
          liveStatus += " (" + String((unsigned)recentReconnects) +
                        (recentReconnects == EVENT_HISTORY_RING_SIZE ? "+" : "") + " in the last 24h)";
        }
      }
      // Hidden behind a click, not shown inline like reconnect/offline
      // counts above - unlike those, this is tuning-diagnostic detail most
      // visits to this page don't need, and showing avg/min/max text mixed
      // into every row's status by default would clutter more than it
      // helps. Plain vanilla JS (no framework), same as the sidebar's own
      // System-submenu toggle (webserver.cpp's renderShell) - toggles one
      // row's own hidden <span>, id'd by rowIdx so every row's toggle is
      // independent. See CameraState::motionLatencyHistory's own comment
      // (camera.h) for what feeds this.
      if (latencyCount > 0) {
        unsigned long avgMs = latencySum / latencyCount;
        String latencyId = "lat" + String((unsigned)rowIdx);
        liveStatus += " <a href=\"#\" onclick=\"var d=document.getElementById('" + latencyId +
                      "');d.style.display=(d.style.display==='inline')?'none':'inline';return false;\" "
                      "title=\"Motion-to-photo latency\">&#9201;</a>";
        // Poll interval shown alongside, not just the measured latency -
        // lets the two be compared at a glance (is the actual latency
        // roughly what this camera's own configured cadence would predict,
        // or is something else - camera-side encode time, network - the
        // bigger contributor) without opening the Edit form to check what's
        // currently set.
        liveStatus += "<span id=\"" + latencyId + "\" style=\"display:none;\"> - last " +
                      String((unsigned)latencyCount) + ": avg " + String(avgMs) + "ms, min " +
                      String(latencyMin) + "ms, max " + String(latencyMax) + "ms (poll interval " +
                      String(c.pollIntervalMs) + "ms)</span>";
      }
      if (hasAlerted) lastAlertStr = formatElapsedSince(lastAlert, millis());

      // cameraSnapshotCount/the "age" param dispatch to whichever backing
      // store is active (SD or the PSRAM ring) - see snapshot_history.h.
      size_t historyCount = cameraSnapshotCount(c, st);
      if (historyCount > 0) {
        // Newest first (age 0). One value shared by this camera's own
        // thumbnails cache-busts them - simpler than a per-entry key, and
        // just as effective: a full page reload always gets a fresh
        // renderMs, so the browser never shows a stale image across page
        // loads, which is all this needs to guarantee. (Different cameras
        // on the same page get different renderMs values too, since this
        // is recomputed per row - harmless, just more cache-busting than
        // strictly required.)
        unsigned long renderMs = millis();
        previewCell = "";
        // Oldest-to-newest, for playFlipbook below - age counts backward
        // from 0 (newest), so chronological playback order is the reverse
        // of this loop's natural age=0..historyCount-1 iteration.
        String flipbookUrls;
        for (size_t age = 0; age < historyCount; age++) {
          String url = "/cameras/snapshot?name=" + urlEncode(c.name) + "&age=" + String((unsigned)age) +
                        "&t=" + String(renderMs);
          // title attribute, not a visible caption - same "icon + native
          // tooltip instead of full text" reasoning as the badges above:
          // this strip is a dense row of 48px thumbnails, no room for a
          // label under each one without breaking the layout. The
          // Gallery page's own, larger grid shows this as visible text
          // instead (webserver_gallery.cpp).
          String sourceLabel = snapshotSourceLabel(cameraSnapshotSourceAt(c, st, age));
          previewCell += "<a href=\"" + url + "\" target=\"_blank\">"
                         "<img src=\"" + url + "\" style=\"max-width:48px;max-height:36px;margin:1px;\" "
                         "alt=\"preview\" title=\"" + sourceLabel + "\"></a>";
          String oldestFirstUrl = "/cameras/snapshot?name=" + urlEncode(c.name) +
                                   "&age=" + String((unsigned)(historyCount - 1 - age)) + "&t=" + String(renderMs);
          flipbookUrls += (age > 0 ? "|" : "") + oldestFirstUrl;
        }
        // Flipbook only makes sense with more than one frame - already
        // loaded as the thumbnails above, so playback costs no extra
        // requests (same URLs, browser cache). See playFlipbook's own
        // comment (this panel's shared <script>, below the table) for the
        // play/pause toggle itself.
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

    // Icon + native title-attribute tooltip instead of the full text, so a
    // long note doesn't blow out the column width - no JS needed, the
    // browser renders the tooltip on hover itself.
    String notesCell = c.notes.length() > 0
        ? "<span title=\"" + htmlEscape(c.notes) + "\" style=\"cursor:help;\">\xF0\x9F\x93\x9D</span>"
        : "";

    String enabledBadge = c.enabled ? "<span class=\"badge badge-on\">yes</span>"
                                      : "<span class=\"badge badge-off\">no</span>";
    html += "<tr><td>" + htmlEscape(c.name) + "</td><td>" + htmlEscape(c.deviceServiceUrl) +
            "</td><td>" + enabledBadge + "</td><td>" + liveStatus + "</td><td>" +
            lastAlertStr + "</td><td>" + previewCell + "</td><td>" +
            notesCell + "</td><td><div class=\"row-actions\">";
    // Best-effort convenience link to the camera's own web UI (its plain
    // http:// root on the default port, not the ONVIF service path/port) -
    // deviceServiceUrl is the only address on record for it, so this is a
    // guess, not a guarantee every camera actually serves a UI there.
    // Deliberately drops any port from deviceServiceUrl rather than
    // reusing it: several real cameras in the field expose ONVIF on a
    // non-standard port specifically because it's a bolted-on service
    // (e.g. Vstarcam's onvif_support_enable on :10080) while their actual
    // embedded admin UI stays on the standard port 80 - reusing the ONVIF
    // port would link to a dead/wrong port far more often than defaulting
    // to 80 does. Opens in a new tab so a dead link doesn't navigate the
    // dashboard away.
    String hostOnly = extractHost(c.deviceServiceUrl);
    int portSep = hostOnly.indexOf(':');
    if (portSep >= 0) hostOnly = hostOnly.substring(0, portSep);
    // htmlEscape() here is load-bearing, not defensive boilerplate:
    // deviceServiceUrl is attacker-controllable two ways that bypass any
    // admin typing entirely - a rogue camera's WS-Discovery ProbeMatch
    // reply (renderCameraDiscoveryStatus, this file) feeds it directly,
    // and Import (webserver_security.cpp's applyConfigImport) writes it
    // from an uploaded file with only a non-empty check. Unescaped, either
    // one plants a stored XSS payload that fires in the admin's own
    // session on every future page load.
    html += "<a class=\"icon-btn secondary\" href=\"http://" + htmlEscape(hostOnly) +
            "/\" target=\"_blank\" title=\"Open camera's web UI\" aria-label=\"Open camera's web UI\">"
            "&#8599;</a>";
    // RTSP live-view link, only once the standard ONVIF flow has actually
    // resolved a stream URI (cameraFetchProfileAndSnapshotUri, camera.cpp) -
    // see CameraState::streamUri's own comment for why a camera using
    // snapshotUriOverride never gets one here. Credentials embedded
    // (rtsp://user:pass@host/...) so this is directly pastable into VLC/an
    // NVR without the viewer having to already know them - the same
    // credentials cfg.user/cfg.pass already stores in NVS, not a new
    // exposure. htmlEscape() here is load-bearing, not defensive
    // boilerplate: rawUri came back from this camera's own SOAP response,
    // which - like deviceServiceUrl above - a rogue/compromised camera
    // controls entirely.
    if (rtspUri.length() > 0) {
      String rtspWithCreds = buildUriWithCredentials(rtspUri, c.user, c.pass);
      html += " <a class=\"icon-btn secondary\" href=\"" + htmlEscape(rtspWithCreds) +
              "\" title=\"Open live RTSP stream (e.g. in VLC)\" aria-label=\"Open live RTSP stream\">"
              "&#9654;</a>";
    }
    // Inline MJPEG live preview - only once this camera's own
    // VideoEncoderConfiguration actually reports a JPEG-encoded profile
    // AND the camera accepted HTTP transport for it (see
    // CameraState::mjpegUri's own comment, camera.h, for how rare that
    // combination is - most modern H.264/H.265-only cameras never get
    // here at all). Unlike the RTSP link above, a plain <img> tag can
    // stream this directly with zero browser plugin/codec - toggled
    // on/off rather than always-on, since the browser holds a real,
    // sustained HTTP connection straight to the camera for as long as
    // it's showing (bypassing the ESP32 entirely - zero load on this
    // board either way), which can contend with this project's own ONVIF
    // polling on a camera whose embedded HTTP stack only tolerates one or
    // two concurrent connections (see CameraState::snapshotInFlight's own
    // comment for a real instance of that class of limit).
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
      // Own <form>, not just a link - this is a real POST that fetches a
      // fresh snapshot and sends it to Telegram, not a passive navigation.
      // Fires and returns immediately (startTestAlertAsync, background
      // task) - see that function's own comment for why this can't run
      // synchronously on this request-handling task. The kind selector
      // lets the test embed the same Person/Vehicle/Pet emoji/keyword a
      // real detection's caption would, so a phone-side notification
      // automation (MacroDroid/Tasker) can be verified without waiting
      // for an actual person, vehicle, or pet - see trTestAlertCaption's
      // own comment (telegram_i18n.h). Pet is a separate isPetEvent flag,
      // not a MotionDetectionKind value - same split as the real alert
      // path (triggerMotionAlert) - so it's threaded alongside kind, not
      // through it.
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

  // Shared by every row's Play button (onclick="playFlipbook(this, '...')")
  // - cycles a camera's own Preview thumbnails (already loaded as <img>s
  // above, so this costs no extra network requests, just browser-cached
  // reads) into one larger image next to the button, oldest-to-newest, so
  // motion across the history is actually visible instead of five static
  // frames you have to open one at a time. Click again to stop; the last
  // frame shown just stays put rather than reverting to the button.
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
          // Shared by every row's Live button (onclick="toggleLivePreview(this,
          // rowId, imgId, src)") - shows/hides the dedicated <tr> this camera's
          // row emitted right after itself, and sets/clears the <img>'s src to
          // actually start/stop the browser's MJPEG-over-HTTP connection to the
          // camera (an empty src doesn't just hide it, it releases the
          // connection - see this button's own title text for why that matters
          // on some cameras).
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

    html += "<fieldset><legend>Set person/vehicle/pet alerts for all cameras</legend>";
    html += "<form method=\"POST\" action=\"/cameras/person-alerts-all\" "
            "onsubmit=\"return confirm('Overwrite every camera\\'s individual PERSON alert setting with "
            "this one? Vehicle and pet alert settings are left untouched. There is no way to see what "
            "each camera currently has before this replaces it.');\">";
    html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\" checked> "
            "Alert on person detection</label>";
    html += "<p><button type=\"submit\">Apply person alerts to all cameras</button></p></form>";
    html += "<form method=\"POST\" action=\"/cameras/vehicle-alerts-all\" "
            "onsubmit=\"return confirm('Overwrite every camera\\'s individual VEHICLE alert setting with "
            "this one? Person and pet alert settings are left untouched. There is no way to see what "
            "each camera currently has before this replaces it.');\">";
    html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\" checked> "
            "Alert on vehicle detection</label>";
    html += "<p><button type=\"submit\">Apply vehicle alerts to all cameras</button></p></form>";
    html += "<form method=\"POST\" action=\"/cameras/pet-alerts-all\" "
            "onsubmit=\"return confirm('Overwrite every camera\\'s individual PET alert setting with this "
            "one? Person and vehicle alert settings are left untouched. There is no way to see what each "
            "camera currently has before this replaces it.');\">";
    // Unchecked by default, unlike the person/vehicle buttons above -
    // matches CameraConfig::petAlertsEnabled's own opt-in-off default
    // (a person/vehicle-only camera would otherwise start paging you for
    // your own pet the moment this button gets clicked without looking).
    html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"> "
            "Alert on pet (dog/cat) detection</label>";
    html += "<p><button type=\"submit\">Apply pet alerts to all cameras</button></p></form>";
    html += "<p class=\"hint\">Each button only overwrites its own setting (person, vehicle, or pet) "
            "across every camera (both enabled and disabled ones) - the other two stay whatever each "
            "camera already has, so bulk-setting one never undoes a deliberate per-camera choice on "
            "another. There's no per-camera preview here, so check each camera's own Edit form afterward "
            "if you need to confirm what landed. Person/vehicle only affects cameras whose own ONVIF AI "
            "actually distinguishes that detection - a plain motion sensor always alerts regardless.</p>";
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
  // A blank/zero/negative field shouldn't produce a 0ms cooldown (alerts on
  // every single poll) - fall back to CameraConfig's own default instead.
  // Upper-capped at CAMERA_ALERT_COOLDOWN_MAX_MS (config.h, 24h): unsigned
  // long is 32-bit on this platform, and *1000UL overflows/wraps above
  // ~4,294,967s - a fat-fingered huge cooldown would otherwise silently
  // wrap into a tiny one (alert spam instead of the throttling actually
  // requested), same overflow class motionWatchdogHours/timelapseIntervalMin
  // below are already clamped against. This is a form-input sanity bound,
  // not the only guard - see telegram.cpp's safeAlertCooldownMs for the
  // point-of-use clamp that also covers a value that bypassed this form
  // entirely (a hand-edited/imported NVS blob).
  long cooldownMaxSec = (long)(CAMERA_ALERT_COOLDOWN_MAX_MS / 1000UL);
  if (cooldownSec > cooldownMaxSec) cooldownSec = cooldownMaxSec;
  c.alertCooldownMs = cooldownSec > 0 ? (unsigned long)cooldownSec * 1000UL : CameraConfig().alertCooldownMs;

  long offlineMin = request->getParam("offlineThresholdMin", "5").toInt();
  // Upper-capped at CAMERA_OFFLINE_THRESHOLD_MAX_MS (7 days), same overflow
  // reasoning as above - *60000UL wraps above ~71583 minutes. See
  // telegram.cpp's safeOfflineThresholdMs for the point-of-use clamp.
  long offlineMaxMin = (long)(CAMERA_OFFLINE_THRESHOLD_MAX_MS / 60000UL);
  if (offlineMin > offlineMaxMin) offlineMin = offlineMaxMin;
  c.offlineThresholdMs = offlineMin > 0 ? (unsigned long)offlineMin * 60000UL : CameraConfig().offlineThresholdMs;

  long burstCount = request->getParam("snapshotBurstCount", "1").toInt();
  // Clamp to [1, CAMERA_SNAPSHOT_BURST_MAX]: blank/zero/negative falls back
  // to 1 shot, and the cap stops a fat-fingered number from flooding past
  // Telegram's rate limit. See telegram.cpp's safeSnapshotBurstCount for
  // the point-of-use clamp.
  if (burstCount < 1) burstCount = 1;
  if (burstCount > (long)CAMERA_SNAPSHOT_BURST_MAX) burstCount = (long)CAMERA_SNAPSHOT_BURST_MAX;
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
  c.timelapseSendToTelegram = request->hasParam("timelapseSendToTelegram");

  // 0 is the deliberate, meaningful "use the global Storage-page setting"
  // value here - same "never substitute it away, only clamp the ceiling"
  // reasoning as motionWatchdogHours/timelapseIntervalMin above.
  long retentionDays = request->getParam("retentionDays", "0").toInt();
  if (retentionDays < 0) retentionDays = 0;
  if (retentionDays > (long)SD_RETENTION_MAX_DAYS) retentionDays = (long)SD_RETENTION_MAX_DAYS;
  c.retentionDays = (uint16_t)retentionDays;

  c.personAlertsEnabled = request->hasParam("personAlertsEnabled");
  c.vehicleAlertsEnabled = request->hasParam("vehicleAlertsEnabled");
  c.petAlertsEnabled = request->hasParam("petAlertsEnabled");
  c.petAlertsTextOnly = request->hasParam("petAlertsTextOnly");
  c.motionDigestEnabled = request->hasParam("motionDigestEnabled");

  // 0 is the deliberate, meaningful "unset - no {WIDTH}/{HEIGHT}
  // substitution" value here - same "never substitute it away, only clamp
  // the ceiling" reasoning as motionWatchdogHours/timelapseIntervalMin
  // above. camera.cpp's safeSnapshotDimension re-clamps at the point of
  // use for a value that bypassed this form entirely.
  long snapshotMaxWidth = request->getParam("snapshotMaxWidth", "0").toInt();
  if (snapshotMaxWidth < 0) snapshotMaxWidth = 0;
  if (snapshotMaxWidth > (long)CAMERA_SNAPSHOT_DIMENSION_MAX) snapshotMaxWidth = (long)CAMERA_SNAPSHOT_DIMENSION_MAX;
  c.snapshotMaxWidth = (uint16_t)snapshotMaxWidth;

  long snapshotMaxHeight = request->getParam("snapshotMaxHeight", "0").toInt();
  if (snapshotMaxHeight < 0) snapshotMaxHeight = 0;
  if (snapshotMaxHeight > (long)CAMERA_SNAPSHOT_DIMENSION_MAX) snapshotMaxHeight = (long)CAMERA_SNAPSHOT_DIMENSION_MAX;
  c.snapshotMaxHeight = (uint16_t)snapshotMaxHeight;

  // A blank/zero/negative field shouldn't produce a near-0 interval that
  // hammers the camera (see CAMERA_POLL_INTERVAL_MIN_MS's own comment,
  // config.h) - fall back to CameraConfig's own default instead, same
  // reasoning as alertCooldownSec/offlineThresholdMin above. A positive
  // value that's merely too low is clamped UP to the floor rather than
  // discarded outright - the user clearly wants it fast, just not unsafely
  // so. This is a form-input sanity bound, not the only guard - see
  // camera.cpp's safePollIntervalMs for the point-of-use clamp that also
  // covers a value that bypassed this form entirely (a hand-edited/
  // imported NVS blob).
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
    // A brand new camera has no slot in liveCameras/liveStates yet -
    // stagePendingNewCamera (camera_tasks.h) gives it one live, applied by
    // loop()'s own task on its very next tick (typically well under a
    // second), UNLESS this board's reserved camera capacity (MAX_CAMERAS,
    // config.h) is already used up, in which case it declines to stage
    // anything and this falls back to the original "needs a reboot"
    // outcome - said explicitly in the banner now, rather than silently
    // doing nothing.
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
    // Deliberately NOT htmlEscape()d here - applyNote rides a URL-encoded
    // redirect query param and is escaped exactly once, at the single
    // point it's rendered (the /cameras GET handler). Escaping it here too
    // used to seem like defense in depth, but created a real reflected-XSS
    // hole: the GET handler trusted the pre-escaping and skipped its own,
    // so a direct /cameras?note=<script>...</script> request (bypassing
    // this POST flow) rendered completely unescaped. One escaping point.
    if (wasRunning && cam.enabled) {
      // Still enabled before and after - stage the new config for the
      // already-running task to pick up itself. See requestLiveConfigReload
      // (camera.h) for why this webserver task must never write the live
      // CameraConfig's String fields directly.
      requestLiveConfigReload((*liveStates)[idx], cam);
      applyNote = "\"" + cam.name + "\" updated - applying live, reconnecting now (no reboot needed).";
    } else if (!wasRunning && cam.enabled) {
      // Was disabled (or never got a task at boot) and this edit enabled
      // it. Staged via the same pendingConfig mechanism a running camera's
      // edit uses, not written into (*liveCameras)[idx] directly - other
      // tasks may read cameras[i].enabled/.name without a lock the instant
      // it flips to enabled, and CameraConfig itself has no locking of its
      // own. cameraStateInit() first, so that lock exists to mean anything.
      cameraStateInit((*liveStates)[idx]);
      requestLiveConfigReload((*liveStates)[idx], cam);
      spawnCameraTask(idx); // its own startup applies the staged config - see applyPendingConfigIfAny
      logEvent(cam.name + ": enabled via dashboard, monitoring started live");
      applyNote = "\"" + cam.name + "\" enabled - monitoring started live (no reboot needed).";
    } else if (wasRunning && !cam.enabled) {
      // Every other field change here (if any) is discarded along with the
      // task itself - a disable takes priority over any other edit in the
      // same save, same as requestCameraStop winning over a pending config
      // reload if both were somehow staged at once (see cameraTaskFn).
      requestCameraStop((*liveStates)[idx]);
      applyNote = "\"" + cam.name + "\" disabled - monitoring stopped live (no reboot needed).";
    }
    // else: wasn't running, still not enabled - nothing live to do.
  }
  // else: idx < 0 - this camera isn't in liveCameras at all (added to NVS
  // after this board's current boot) - still needs a reboot to get a live
  // slot in the first place, same as always (TODO in camera_tasks.h).

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

  // Every camera gets this, not just enabled ones, so a currently-disabled
  // camera's stored config stays consistent if it's enabled later.
  // updateAllCameras() (camera_store.h) loads, mutates, and saves all
  // under one mutex hold, unlike a separate loadCameras()+replaceAllCameras()
  // sequence - which would only protect the final write, leaving the read
  // in between free to race a concurrent single-camera edit and silently
  // discard it when this write lands second.
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

  // Live-reloads every already-running enabled camera, same as a single-
  // camera edit already does (saveCameraSubmission's wasRunning&&cam.enabled
  // branch) - full rediscovery per camera, the same cost editing each one
  // individually would have, just triggered by one click instead of N. A
  // fresh read (the write above has already committed by the time we're
  // here, so this sees it) rather than reusing the mutated copy from
  // inside updateAllCameras' own critical section - simpler than changing
  // that function's signature just to hand the list back out.
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

// Same shape as applyQuietHoursToAllCameras above - see its own comments
// for the reasoning (updateAllCameras' single-critical-section read+
// mutate+save, why every camera gets this not just enabled ones, and the
// live-reload cost) - just for personAlertsEnabled instead of quiet hours.
//
// Deliberately its own function/route/button rather than one combined
// "person+vehicle" form (an earlier version did that, with both
// checkboxes defaulting to checked) - bulk-applying just one axis then
// silently re-enabled the other for every camera too, which could
// silently undo e.g. a busy-street camera's deliberately-disabled vehicle
// alerts the admin wasn't even thinking about in that click. Each button
// here only ever touches its own field.
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

// Same as applyPersonAlertsToAllCameras above, for vehicleAlertsEnabled -
// see its comment for why this is separate rather than one combined form.
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

// Same as applyPersonAlertsToAllCameras above, for petAlertsEnabled - see
// its comment for why this is separate rather than one combined form.
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

// ============================================================
// Background wrapper - see testCameraConnection's own comment above and
// webserver_cameras.h's startTestConnectionAsync comment for why this
// can't run synchronously on the calling (PsychicHttp) task, and why cfg
// has to be heap-copied into the task rather than read from NVS the way
// testAllCameraConnections/cameraDiscoveryTask read their own inputs.
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

  // Freed by testConnectionTask itself once it's done with it - same
  // ownership pattern as camera_tasks.h's CameraTaskContext.
  CameraConfig* cfgCopy = new CameraConfig(cfg);
  // Same stack size as a real per-camera monitoring task (camera_tasks.h) -
  // this does the identical TLS/HTTPClient/SOAP-string-building work,
  // just for one camera's Test Connection sequence instead of forever.
  BaseType_t created = xTaskCreate(testConnectionTask, "testConn", 10240, cfgCopy, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // tryStart() already committed to inProgress=true above - without
    // this, a task creation failure (out of memory) would leave it stuck
    // that way forever, since nothing will ever call g_testConnectionJob's
    // finish() for a task that never launched. See BackgroundJob<T>::
    // cancelStart's own comment (background_job.h).
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
// Send Test Alert background wrapper - see webserver_cameras.h's
// startTestAlertAsync comment and telegram.h's sendTestAlert for why this
// can't run synchronously on the calling (PsychicHttp) task.
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
  // Same stack size as the other camera background tasks above - comparable
  // work (one HTTP fetch, one or more TLS sends to Telegram).
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

// Runs a read-only subset of testCameraConnection's ONVIF call sequence,
// packaged as a condensed CameraTestResult - a separate function rather
// than reusing testCameraConnection because its caller
// (testAllCameraConnections) needs a compact, uniform shape for a
// one-row-per-camera table, not a differentiated prose paragraph.
// Deliberately never calls cameraCreatePullPoint - see CameraTestResult's
// header comment for why creating a second subscription on every already-
// monitored camera at once is a bigger risk than the single-camera
// version's deliberate one-at-a-time use.
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

// Tests every ENABLED camera currently persisted in NVS, one after
// another. Runs on its own background FreeRTOS task (startTestAllCamerasAsync
// below), not the calling task directly - PsychicHttp here services one
// request at a time (no async worker pool), so running this synchronously
// would make the entire dashboard unreachable for every other request, not
// just slow the clicker, for as long as the test takes (can be minutes
// with several cameras down).
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
// Background wrapper - see testAllCameraConnections' own comment for why
// this can't just run synchronously on the calling (PsychicHttp) task.
// BackgroundJob<T> (background_job.h) owns the mutex/state-machine part -
// shared with cameraDiscoveryTask's own wrapper further down, which needs
// the identical "start unless already running / finish / poll status"
// shape for an unrelated result type.
// ============================================================

static BackgroundJob<std::vector<CameraTestResult>> g_testAllJob;

static void testAllCamerasTask(void*) {
  g_testAllJob.finish(testAllCameraConnections()); // the actual (slow) work
  vTaskDelete(nullptr);
}

BackgroundJobStartOutcome startTestAllCamerasAsync() {
  // Deferred, not started-then-blocked: a per-camera SOAP burst overlapping
  // an in-flight Telegram photo send's JPEG+TLS buffers is exactly the
  // class of coincidence that has driven free heap dangerously low in the
  // field - see telegramSendInProgress's own comment (telegram.h). Reusing
  // AlreadyRunning here (rather than a new outcome/banner text) is
  // deliberate: from the person who clicked the button, "try again in a
  // moment" reads the same either way, and this check re-runs fresh on
  // every click - no risk of getting stuck waiting for a send that already
  // finished by the time they try again.
  if (telegramSendInProgress()) return BackgroundJobStartOutcome::AlreadyRunning;
  if (!g_testAllJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one run at a time - a second click while one's in flight is a no-op

  // Same stack size as a real per-camera monitoring task (camera_tasks.h) -
  // this does the identical TLS/HTTPClient/SOAP-string-building work per
  // camera, just for several cameras in a row instead of one forever.
  BaseType_t created = xTaskCreate(testAllCamerasTask, "testAllCams", 10240, nullptr, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // See startTestConnectionAsync's own comment above - without this, a
    // task creation failure here would leave g_testAllJob permanently
    // stuck "in progress".
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
      // Was a merged colspan="2" cell before switching to the shared
      // renderDataTable (webserver_html.h) - same information, one cosmetic
      // difference: "Skipped (disabled)" no longer visually spans both the
      // Reachable and Event service columns, just sits in the first one.
      rows.push_back({r.name, "Skipped (disabled)", "", ""});
      continue;
    }
    rows.push_back({r.name, r.reachable ? "OK" : "FAIL", r.eventServiceOk ? "OK" : "FAIL", r.detail});
  }
  return "<p>Test all cameras - results:</p>" +
         renderDataTable({"Camera", "Reachable", "Event service", "Detail"}, rows);
}

// ============================================================
// Network camera discovery (WS-Discovery) - see webserver_cameras.h's own
// comments on startCameraDiscoveryAsync/renderCameraDiscoveryStatus.
// ============================================================

static const IPAddress kWsDiscoveryMulticastAddr(239, 255, 255, 250);
static const uint16_t  kWsDiscoveryPort = 3702;

// Total time spent listening for ProbeMatch replies after sending the
// probe(s). Long enough for a slower/busier camera to answer (WS-
// Discovery has no guaranteed response time) - widened from an initial
// 4000ms after a real fleet found only 2 of several configured cameras
// with ONVIF Device Manager (a longer-running, wired-PC tool) finding all
// of them: every already-added camera here already has an active ONVIF
// PullPoint subscription held open by this same board, and several cheap
// embedded ONVIF stacks (the XM530-derived kind this project targets)
// are known to go quiet on WS-Discovery while "busy" serving one - not
// something a longer window can fix on its own, but a camera that's just
// slow to answer (rather than deliberately silent) now gets a real chance
// to. The auto-refresh poll (webserver.cpp's renderShell) re-checks every
// 2s regardless of how long this is, so the button still doesn't feel
// broken even at this length.
static const unsigned long kDiscoveryListenMs = 12000;

// UDP has no delivery guarantee, and a multicast probe is exactly the kind
// of packet a busy Wi-Fi segment can drop - sending it more than once
// (spaced out across the listen window, not back-to-back) catches a
// camera that missed the first one without meaningfully lengthening the
// wait, since replies from the first send are still being collected while
// later sends go out. Scaled up alongside kDiscoveryListenMs so the
// re-probe spacing (still 1/sec - see the send loop below) stays roughly
// the same fraction of the total window, rather than front-loading every
// probe into the first few seconds and leaving the rest of a much longer
// window with no further retries.
static const int kDiscoveryProbeCount = 8;

static BackgroundJob<std::vector<DiscoveredCamera>> g_discoveryJob;

// The actual (slow) work - runs on discoveryTask's own background task,
// never on the calling task. See kDiscoveryListenMs's comment for why this
// blocks for several seconds by design.
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
  // Deferred, not started-then-blocked - see startTestAllCamerasAsync's
  // identical check above for the full reasoning (telegramSendInProgress,
  // telegram.h): this is the specific job a real field incident traced a
  // near-heap-exhaustion event to (its multi-second UDP listen window
  // overlapping an in-flight Telegram photo send).
  if (telegramSendInProgress()) return BackgroundJobStartOutcome::AlreadyRunning;
  if (!g_discoveryJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one search at a time - a second click while one's in flight is a no-op

  // No TLS/HTTPClient work here (unlike the per-camera and test-all
  // tasks), so a smaller stack than their 10240 is enough - but
  // runCameraDiscovery's own on-stack `char buf[2048]` alone is half of a
  // 4096 stack, leaving too little headroom for the String churn in
  // buildProbeMessage/parseProbeMatch plus WiFiUDP's own call frames.
  // 6144 keeps the "lighter than the TLS tasks" sizing while actually
  // covering that buffer.
  BaseType_t created = xTaskCreate(cameraDiscoveryTask, "camDiscover", 6144, nullptr, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // See startTestConnectionAsync's own comment above - without this, a
    // task creation failure here would leave g_discoveryJob permanently
    // stuck "in progress".
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
