#include "camera_form.h"
#include "config.h"
#include "format_utils.h"
#include <cctype>

String minutesToHHMM(uint16_t minutes) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(minutes / 60) % 24, (unsigned)(minutes % 60));
  return String(buf);
}

// "HH:MM" -> minutes since midnight (toInt() would stop at the colon). 0 on
// malformed input, which quiet hours treat as no window.
uint16_t parseHHMMToMinutes(const String& hhmm) {
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

// Add (fresh defaults), Edit (stored record, password blanked), or a redisplay
// after Test Connection.
String renderCameraForm(const CameraConfig& v, bool isEdit) {
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

CameraConfig parseCameraForm(const FormParams& params) {
  CameraConfig c;
  c.name                          = params.get("name", "");
  c.deviceServiceUrl              = params.get("deviceServiceUrl", "");
  c.enabled                       = params.has("enabled");
  c.useWSSecurity                 = params.has("useWSSecurity");
  c.includeInitialTerminationTime = params.has("includeInitialTerminationTime");
  c.includeReplyToAnonymous       = params.has("includeReplyToAnonymous");
  c.snapshotUriOverride           = params.get("snapshotUriOverride", "");
  c.preferredProfileKeyword       = params.get("preferredProfileKeyword", "");
  c.user                          = params.get("user", "");
  c.pass                          = params.get("pass", "");
  c.notes                         = params.get("notes", "");
  c.name.trim();

  long cooldownSec = params.get("alertCooldownSec", "30").toInt();
  // Blank/zero falls back to the default (0 would alert every poll). Capped at
  // 24h: seconds * 1000 overflows 32 bits. Re-clamped at use too.
  long cooldownMaxSec = (long)(CAMERA_ALERT_COOLDOWN_MAX_MS / 1000UL);
  if (cooldownSec > cooldownMaxSec) cooldownSec = cooldownMaxSec;
  c.alertCooldownMs = cooldownSec > 0 ? (unsigned long)cooldownSec * 1000UL : CameraConfig().alertCooldownMs;

  long offlineMin = params.get("offlineThresholdMin", "5").toInt();
  // Capped for the same overflow (minutes * 60000).
  long offlineMaxMin = (long)(CAMERA_OFFLINE_THRESHOLD_MAX_MS / 60000UL);
  if (offlineMin > offlineMaxMin) offlineMin = offlineMaxMin;
  c.offlineThresholdMs = offlineMin > 0 ? (unsigned long)offlineMin * 60000UL : CameraConfig().offlineThresholdMs;

  long burstCount = params.get("snapshotBurstCount", "1").toInt();
  // 1..CAMERA_SNAPSHOT_BURST_MAX, so a typo can't flood Telegram.
  if (burstCount < 1) burstCount = 1;
  if (burstCount > (long)CAMERA_SNAPSHOT_BURST_MAX) burstCount = (long)CAMERA_SNAPSHOT_BURST_MAX;
  c.snapshotBurstCount = (unsigned int)burstCount;

  c.quietHoursEnabled = params.has("quietHoursEnabled");
  c.quietStartMinute  = parseHHMMToMinutes(params.get("quietStart", "00:00"));
  c.quietEndMinute    = parseHHMMToMinutes(params.get("quietEnd", "00:00"));

  // 0 means off here, so only clamp negatives and the ceiling.
  long watchdogHours = params.get("motionWatchdogHours", "0").toInt();
  if (watchdogHours < 0) watchdogHours = 0;
  if (watchdogHours > 168) watchdogHours = 168; // 1 week
  c.motionWatchdogHours = (uint16_t)watchdogHours;

  long timelapseMin = params.get("timelapseIntervalMin", "0").toInt();
  if (timelapseMin < 0) timelapseMin = 0;
  if (timelapseMin > 1440) timelapseMin = 1440; // 24h
  c.timelapseIntervalMin = (uint16_t)timelapseMin;
  c.timelapseSendToTelegram = params.has("timelapseSendToTelegram");

  // 0 means "use the global setting".
  long retentionDays = params.get("retentionDays", "0").toInt();
  if (retentionDays < 0) retentionDays = 0;
  if (retentionDays > (long)SD_RETENTION_MAX_DAYS) retentionDays = (long)SD_RETENTION_MAX_DAYS;
  c.retentionDays = (uint16_t)retentionDays;

  c.personAlertsEnabled = params.has("personAlertsEnabled");
  c.vehicleAlertsEnabled = params.has("vehicleAlertsEnabled");
  c.petAlertsEnabled = params.has("petAlertsEnabled");
  c.petAlertsTextOnly = params.has("petAlertsTextOnly");
  c.motionDigestEnabled = params.has("motionDigestEnabled");

  // 0 means unset (no substitution).
  long snapshotMaxWidth = params.get("snapshotMaxWidth", "0").toInt();
  if (snapshotMaxWidth < 0) snapshotMaxWidth = 0;
  if (snapshotMaxWidth > (long)CAMERA_SNAPSHOT_DIMENSION_MAX) snapshotMaxWidth = (long)CAMERA_SNAPSHOT_DIMENSION_MAX;
  c.snapshotMaxWidth = (uint16_t)snapshotMaxWidth;

  long snapshotMaxHeight = params.get("snapshotMaxHeight", "0").toInt();
  if (snapshotMaxHeight < 0) snapshotMaxHeight = 0;
  if (snapshotMaxHeight > (long)CAMERA_SNAPSHOT_DIMENSION_MAX) snapshotMaxHeight = (long)CAMERA_SNAPSHOT_DIMENSION_MAX;
  c.snapshotMaxHeight = (uint16_t)snapshotMaxHeight;

  // Blank/zero falls back to the default; a too-low value is raised to the
  // floor rather than discarded. Re-clamped at use too.
  long pollIntervalMs = params.get("pollIntervalMs", "2000").toInt();
  if (pollIntervalMs <= 0) pollIntervalMs = (long)CameraConfig().pollIntervalMs;
  if (pollIntervalMs < (long)CAMERA_POLL_INTERVAL_MIN_MS) pollIntervalMs = (long)CAMERA_POLL_INTERVAL_MIN_MS;
  if (pollIntervalMs > (long)CAMERA_POLL_INTERVAL_MAX_MS) pollIntervalMs = (long)CAMERA_POLL_INTERVAL_MAX_MS;
  c.pollIntervalMs = (unsigned long)pollIntervalMs;

  return c;
}
