#include "camera_form.h"
#include "config.h"
#include "format_utils.h"
#include "webserver_html.h"
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

namespace {

// ============================================================
// The form as data: one entry per form element, in page order. Rendering and
// parsing both walk this table, so adding a field is one entry.
// ============================================================

enum class Kind { Text, Password, Checkbox, Time, Number, Html, Custom };

struct Field {
  Kind kind;
  const char* name = "";
  const char* label = "";          // trusted HTML
  String (*labelFn)() = nullptr;    // for labels that embed constants

  // Text
  String CameraConfig::*text = nullptr;
  const char* attrs = "";
  bool trim = false;
  // Checkbox
  bool CameraConfig::*flag = nullptr;
  // Time
  uint16_t CameraConfig::*minutes = nullptr;
  // Number: shown as stored / scale. Parsing: a non-positive entry means the
  // default when useDefaultIfNonPositive, then clamp to [min, max].
  unsigned long (*getNum)(const CameraConfig&) = nullptr;
  void (*setNum)(CameraConfig&, unsigned long) = nullptr;
  unsigned long scale = 1;
  long min = 0, max = 0;
  bool useDefaultIfNonPositive = false;
  bool blankWhenZero = false;
  // Html / Custom
  String (*custom)(const CameraConfig&) = nullptr;
};

template <typename T, T CameraConfig::*M>
unsigned long getMember(const CameraConfig& c) { return (unsigned long)(c.*M); }
template <typename T, T CameraConfig::*M>
void setMember(CameraConfig& c, unsigned long v) { c.*M = (T)v; }
#define NUM_MEMBER(m) &getMember<decltype(CameraConfig::m), &CameraConfig::m>, \
                      &setMember<decltype(CameraConfig::m), &CameraConfig::m>

constexpr Field text(const char* name, const char* label, String CameraConfig::*m, const char* attrs = "",
                     bool trim = false) {
  Field f{};
  f.kind = Kind::Text;
  f.name = name; f.label = label; f.text = m; f.attrs = attrs; f.trim = trim;
  return f;
}

constexpr Field password() {
  Field f{};
  f.kind = Kind::Password;
  return f;
}

constexpr Field checkbox(const char* name, const char* label, bool CameraConfig::*m) {
  Field f{};
  f.kind = Kind::Checkbox;
  f.name = name; f.label = label; f.flag = m;
  return f;
}

constexpr Field timeOfDay(const char* name, const char* label, uint16_t CameraConfig::*m) {
  Field f{};
  f.kind = Kind::Time;
  f.name = name; f.label = label; f.minutes = m;
  return f;
}

constexpr Field number(const char* name, const char* label, unsigned long (*get)(const CameraConfig&),
                       void (*set)(CameraConfig&, unsigned long), unsigned long scale, long min, long max,
                       bool useDefaultIfNonPositive, bool blankWhenZero = false) {
  Field f{};
  f.kind = Kind::Number;
  f.name = name; f.label = label; f.getNum = get; f.setNum = set; f.scale = scale;
  f.min = min; f.max = max; f.useDefaultIfNonPositive = useDefaultIfNonPositive; f.blankWhenZero = blankWhenZero;
  return f;
}

constexpr Field withLabel(Field f, String (*fn)()) {
  f.labelFn = fn;
  return f;
}

String widthLabel() {
  return "Snapshot width override, for {WIDTH} above (optional, max " + String(CAMERA_SNAPSHOT_DIMENSION_MAX) +
         ", blank/0 = unset)";
}

String heightLabel() {
  return "Snapshot height override, for {HEIGHT} above (optional, max " + String(CAMERA_SNAPSHOT_DIMENSION_MAX) +
         ", blank/0 = unset)";
}

String pollLabel() {
  return "Poll interval, ms, " + String(CAMERA_POLL_INTERVAL_MIN_MS) + "-" + String(CAMERA_POLL_INTERVAL_MAX_MS) +
         " (how often this camera is asked \"anything new?\" - lower notices motion sooner, at the cost of more "
         "frequent requests to this camera; some cheaper cameras' embedded HTTP stacks tolerate that worse than "
         "others)";
}

constexpr Field html(const char* markup) {
  Field f{};
  f.kind = Kind::Html;
  f.label = markup;
  return f;
}

constexpr Field custom(String (*fn)(const CameraConfig&)) {
  Field f{};
  f.kind = Kind::Custom;
  f.custom = fn;
  return f;
}

// Motion during quiet hours still feeds the watchdog clock, so a watchdog
// shorter than the quiet window would false-alarm; warn about the combo.
String watchdogVsQuietHoursWarning(const CameraConfig& v) {
  if (!v.quietHoursEnabled || v.quietStartMinute == v.quietEndMinute || v.motionWatchdogHours == 0) return "";
  int windowMin = (v.quietEndMinute > v.quietStartMinute)
      ? (v.quietEndMinute - v.quietStartMinute)
      : (1440 - v.quietStartMinute + v.quietEndMinute); // wraps past midnight
  if ((int)v.motionWatchdogHours * 60 > windowMin) return "";
  return "<p class=\"hint\">\xE2\x9A\xA0\xEF\xB8\x8F The no-motion watchdog (" + String(v.motionWatchdogHours) +
         "h) is shorter than or equal to the quiet hours window (" + String(windowMin / 60) + "h" +
         String(windowMin % 60) + "m) - it will likely trip a false alert every quiet period even though "
         "nothing's actually wrong. Consider raising the watchdog hours above the quiet hours window length.</p>";
}

// constexpr so the table lives in flash, not on the heap.
constexpr Field kFields[] = {
  text("name", "Name (unique)", &CameraConfig::name, " required", true),
  text("deviceServiceUrl", "Device service URL, e.g. http://192.168.1.50/onvif/device_service",
       &CameraConfig::deviceServiceUrl, " required"),
  text("user", "Username", &CameraConfig::user),
  password(),
  checkbox("enabled", "Enabled", &CameraConfig::enabled),
  checkbox("useWSSecurity", "Use WS-Security (uncheck for HTTP Basic Auth)", &CameraConfig::useWSSecurity),
  checkbox("includeInitialTerminationTime", "Include InitialTerminationTime",
           &CameraConfig::includeInitialTerminationTime),
  checkbox("includeReplyToAnonymous", "Include ReplyTo anonymous", &CameraConfig::includeReplyToAnonymous),
  text("snapshotUriOverride",
       "Snapshot URI override (optional; {USER}/{PASS}/{WIDTH}/{HEIGHT} substituted at runtime)",
       &CameraConfig::snapshotUriOverride),
  withLabel(number("snapshotMaxWidth", "",
         NUM_MEMBER(snapshotMaxWidth), 1, 0, (long)CAMERA_SNAPSHOT_DIMENSION_MAX, false, true), &widthLabel),
  withLabel(number("snapshotMaxHeight", "",
         NUM_MEMBER(snapshotMaxHeight), 1, 0, (long)CAMERA_SNAPSHOT_DIMENSION_MAX, false, true), &heightLabel),
  text("preferredProfileKeyword", "Preferred profile keyword (optional, e.g. \"sub\")",
       &CameraConfig::preferredProfileKeyword),
  // Cooldown/offline: a non-positive entry means the default (0 would
  // alert every poll); the caps stop seconds*1000 / minutes*60000
  // overflowing 32 bits. Re-clamped at use too.
  number("alertCooldownSec",
         "Alert cooldown, seconds, max 86400 (minimum time between Telegram alerts for this camera)",
         NUM_MEMBER(alertCooldownMs), 1000UL, 1, (long)(CAMERA_ALERT_COOLDOWN_MAX_MS / 1000UL), true),
  number("offlineThresholdMin",
         "Offline threshold, minutes, max 10080 (no response for this long -> OFFLINE alert)",
         NUM_MEMBER(offlineThresholdMs), 60000UL, 1, (long)(CAMERA_OFFLINE_THRESHOLD_MAX_MS / 60000UL), true),
  number("snapshotBurstCount",
         "Snapshots per alert (1-10) - how many consecutive photos to send when motion fires, each a fresh "
         "fetch from the camera; raise it to see more of what led up to the alert",
         NUM_MEMBER(snapshotBurstCount), 1, 1, (long)CAMERA_SNAPSHOT_BURST_MAX, false),
  // A too-low poll interval is raised to the floor, not discarded.
  withLabel(number("pollIntervalMs", "",
         NUM_MEMBER(pollIntervalMs), 1, (long)CAMERA_POLL_INTERVAL_MIN_MS, (long)CAMERA_POLL_INTERVAL_MAX_MS,
         true), &pollLabel),
  checkbox("quietHoursEnabled", "Quiet hours (mutes motion alerts only - tamper/offline still alert)",
           &CameraConfig::quietHoursEnabled),
  timeOfDay("quietStart", "Quiet hours start", &CameraConfig::quietStartMinute),
  timeOfDay("quietEnd", "Quiet hours end", &CameraConfig::quietEndMinute),
  html("<p class=\"hint\">Leaving start and end the same (e.g. both 00:00) means no active window - quiet "
       "hours needs a real start/end to do anything.</p>"),
  // For the fields below, 0 is a real setting (off / use global), so
  // only the range is clamped.
  number("motionWatchdogHours",
         "No-motion watchdog, hours (0 = off) - alerts if this camera hasn't seen ANY motion in over this "
         "long, e.g. a dead PIR or a knocked-over camera",
         NUM_MEMBER(motionWatchdogHours), 1, 0, 168, false),
  custom(&watchdogVsQuietHoursWarning),
  number("timelapseIntervalMin",
         "Timelapse capture, minutes (0 = off) - stores a snapshot on this interval regardless of motion, "
         "kept in history/SD",
         NUM_MEMBER(timelapseIntervalMin), 1, 0, 1440, false),
  checkbox("timelapseSendToTelegram",
           "Also send these scheduled snapshots to Telegram (in addition to storing them) - ignored while "
           "the interval above is 0",
           &CameraConfig::timelapseSendToTelegram),
  number("retentionDays", "Snapshot retention override, days (0 = use the Storage page's global setting)",
         NUM_MEMBER(retentionDays), 1, 0, (long)SD_RETENTION_MAX_DAYS, false),
  checkbox("personAlertsEnabled",
           "Alert on person detection - on by default; only uncheck to mute person alerts for this specific "
           "camera while leaving other detection types alone (requires a camera whose own ONVIF AI actually "
           "distinguishes person detection - a plain motion sensor always alerts regardless of this)",
           &CameraConfig::personAlertsEnabled),
  checkbox("vehicleAlertsEnabled",
           "Alert on vehicle detection - on by default; uncheck for a camera facing a busy street that would "
           "otherwise page you for every passing car",
           &CameraConfig::vehicleAlertsEnabled),
  checkbox("petAlertsEnabled",
           "Alert on pet (dog/cat) detection too - off by default, since a person/vehicle-only camera would "
           "otherwise page you for your own pet",
           &CameraConfig::petAlertsEnabled),
  checkbox("petAlertsTextOnly",
           "Send pet alerts as text only, no photo - ignored unless pet alerts above are on",
           &CameraConfig::petAlertsTextOnly),
  checkbox("motionDigestEnabled",
           "Send a follow-up \"motion continued\" summary after a real alert's cooldown ends - on by default; "
           "uncheck for a camera busy enough that the follow-up summary is itself noise on top of the alert "
           "it's summarizing",
           &CameraConfig::motionDigestEnabled),
  text("notes", "Notes", &CameraConfig::notes),
};

String labelOf(const Field& f) { return f.labelFn ? f.labelFn() : String(f.label); }

}  // namespace

// Add (fresh defaults), Edit (stored record, password blanked), or a redisplay
// after Test Connection.
String renderCameraForm(const CameraConfig& v, bool isEdit) {
  String legend = isEdit ? ("Edit camera: " + htmlEscape(v.name)) : "Add camera";
  String out = "<fieldset><legend>" + legend + "</legend><form method=\"POST\" action=\"/cameras/save\">";
  if (isEdit) out += htmlHiddenInput("originalName", v.name);

  for (const Field& f : kFields) {
    switch (f.kind) {
      case Kind::Text:
        out += htmlTextInput(labelOf(f), f.name, v.*f.text, f.attrs);
        break;
      case Kind::Password:
        out += isEdit ? htmlPasswordInput("Password (leave blank to keep the current password)", "pass",
                                          " placeholder=\"(unchanged)\"")
                      : htmlPasswordInput("Password", "pass");
        break;
      case Kind::Checkbox:
        out += htmlCheckbox(labelOf(f), f.name, v.*f.flag);
        break;
      case Kind::Time:
        out += htmlTimeInput(labelOf(f), f.name, minutesToHHMM(v.*f.minutes));
        break;
      case Kind::Number: {
        unsigned long shown = f.getNum(v) / f.scale;
        out += htmlTextInput(labelOf(f), f.name, (f.blankWhenZero && shown == 0) ? String("") : String(shown));
        break;
      }
      case Kind::Html:
        out += f.label;
        break;
      case Kind::Custom:
        out += f.custom(v);
        break;
    }
  }

  out += "<p><button type=\"submit\" formaction=\"/cameras/save\">" + String(isEdit ? "Save changes" : "Add camera") +
         "</button> ";
  out += "<button type=\"submit\" formaction=\"/cameras/test\">Test Connection</button>";
  if (isEdit) out += " <a href=\"/cameras\" class=\"secondary\">Cancel</a>";
  out += "</p></form></fieldset>";
  return out;
}

CameraConfig parseCameraForm(const FormParams& params) {
  const CameraConfig defaults;
  CameraConfig c;
  for (const Field& f : kFields) {
    switch (f.kind) {
      case Kind::Text:
        c.*f.text = params.get(f.name, "");
        if (f.trim) (c.*f.text).trim();
        break;
      case Kind::Password:
        c.pass = params.get("pass", "");
        break;
      case Kind::Checkbox:
        c.*f.flag = params.has(f.name);
        break;
      case Kind::Time:
        c.*f.minutes = parseHHMMToMinutes(params.get(f.name, "00:00"));
        break;
      case Kind::Number: {
        long defaultShown = (long)(f.getNum(defaults) / f.scale);
        long v = params.get(f.name, String(defaultShown).c_str()).toInt();
        if (v <= 0 && f.useDefaultIfNonPositive) v = defaultShown;
        if (v < f.min) v = f.min;
        if (v > f.max) v = f.max;
        f.setNum(c, (unsigned long)v * f.scale);
        break;
      }
      case Kind::Html:
      case Kind::Custom:
        break;
    }
  }
  return c;
}
