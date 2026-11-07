#include <unity.h>
#include "fixtures.h"
#include "html_form_reader.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

// ---- Rendering: what a browser submits, and what the page promises ----

static CameraConfig submit(const CameraConfig& c, bool isEdit) {
  return parseCameraForm(BrowserSubmission(renderCameraForm(c, isEdit)));
}

// Every value distinct, so a field wired to the wrong member can't pass.
void test_round_trip_keeps_every_field(void) {
  CameraConfig c = fixtureEditHostile();
  CameraConfig r = submit(c, true);
  TEST_ASSERT_EQUAL_STRING(c.name.c_str(), r.name.c_str());
  TEST_ASSERT_EQUAL_STRING(c.deviceServiceUrl.c_str(), r.deviceServiceUrl.c_str());
  TEST_ASSERT_EQUAL_STRING(c.user.c_str(), r.user.c_str());
  TEST_ASSERT_EQUAL_STRING(c.snapshotUriOverride.c_str(), r.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_STRING(c.preferredProfileKeyword.c_str(), r.preferredProfileKeyword.c_str());
  TEST_ASSERT_EQUAL_STRING(c.notes.c_str(), r.notes.c_str());
  TEST_ASSERT_EQUAL_UINT16(c.snapshotMaxWidth, r.snapshotMaxWidth);
  TEST_ASSERT_EQUAL_UINT16(c.snapshotMaxHeight, r.snapshotMaxHeight);
  TEST_ASSERT_EQUAL_UINT32(c.alertCooldownMs, r.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(c.offlineThresholdMs, r.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT(c.snapshotBurstCount, r.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT32(c.pollIntervalMs, r.pollIntervalMs);
  TEST_ASSERT_EQUAL_UINT16(c.quietStartMinute, r.quietStartMinute);
  TEST_ASSERT_EQUAL_UINT16(c.quietEndMinute, r.quietEndMinute);
  TEST_ASSERT_EQUAL_UINT16(c.motionWatchdogHours, r.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT16(c.timelapseIntervalMin, r.timelapseIntervalMin);
  TEST_ASSERT_EQUAL_UINT16(c.retentionDays, r.retentionDays);
}

// A swapped table entry reads and writes the same wrong member, so a round trip
// can't see it. These tie each field's name to its setting, in both directions.
void test_each_checkbox_name_controls_its_own_setting(void) {
  const struct { const char* name; bool CameraConfig::*flag; } boxes[] = {
      {"enabled", &CameraConfig::enabled},
      {"useWSSecurity", &CameraConfig::useWSSecurity},
      {"includeInitialTerminationTime", &CameraConfig::includeInitialTerminationTime},
      {"includeReplyToAnonymous", &CameraConfig::includeReplyToAnonymous},
      {"quietHoursEnabled", &CameraConfig::quietHoursEnabled},
      {"timelapseSendToTelegram", &CameraConfig::timelapseSendToTelegram},
      {"personAlertsEnabled", &CameraConfig::personAlertsEnabled},
      {"vehicleAlertsEnabled", &CameraConfig::vehicleAlertsEnabled},
      {"petAlertsEnabled", &CameraConfig::petAlertsEnabled},
      {"petAlertsTextOnly", &CameraConfig::petAlertsTextOnly},
      {"motionDigestEnabled", &CameraConfig::motionDigestEnabled}};
  for (auto& on : boxes) {
    CameraConfig c;
    for (auto& b : boxes) c.*b.flag = false;
    c.*on.flag = true;
    BrowserSubmission sent(renderCameraForm(c, false));
    MapParams only;
    only.values = {{on.name, "on"}};
    CameraConfig parsed = parseCameraForm(only);
    for (auto& b : boxes) {
      bool expected = b.flag == on.flag;
      TEST_ASSERT_EQUAL_MESSAGE(expected, sent.has(b.name), b.name);
      TEST_ASSERT_EQUAL_MESSAGE(expected, parsed.*b.flag, b.name);
    }
  }
}

void test_each_value_field_name_shows_its_own_setting(void) {
  CameraConfig c = fixtureEditHostile();
  BrowserSubmission sent(renderCameraForm(c, true));
  TEST_ASSERT_EQUAL_STRING(c.name.c_str(), sent.get("name", "").c_str());
  TEST_ASSERT_EQUAL_STRING(c.deviceServiceUrl.c_str(), sent.get("deviceServiceUrl", "").c_str());
  TEST_ASSERT_EQUAL_STRING(c.user.c_str(), sent.get("user", "").c_str());
  TEST_ASSERT_EQUAL_STRING(c.snapshotUriOverride.c_str(), sent.get("snapshotUriOverride", "").c_str());
  TEST_ASSERT_EQUAL_STRING(c.preferredProfileKeyword.c_str(), sent.get("preferredProfileKeyword", "").c_str());
  TEST_ASSERT_EQUAL_STRING(c.notes.c_str(), sent.get("notes", "").c_str());
  TEST_ASSERT_EQUAL_STRING("640", sent.get("snapshotMaxWidth", "").c_str());
  TEST_ASSERT_EQUAL_STRING("480", sent.get("snapshotMaxHeight", "").c_str());
  TEST_ASSERT_EQUAL_STRING("45", sent.get("alertCooldownSec", "").c_str());
  TEST_ASSERT_EQUAL_STRING("10", sent.get("offlineThresholdMin", "").c_str());
  TEST_ASSERT_EQUAL_STRING("3", sent.get("snapshotBurstCount", "").c_str());
  TEST_ASSERT_EQUAL_STRING("750", sent.get("pollIntervalMs", "").c_str());
  TEST_ASSERT_EQUAL_STRING("09:30", sent.get("quietStart", "").c_str());
  TEST_ASSERT_EQUAL_STRING("17:05", sent.get("quietEnd", "").c_str());
  TEST_ASSERT_EQUAL_STRING("2", sent.get("motionWatchdogHours", "").c_str());
  TEST_ASSERT_EQUAL_STRING("15", sent.get("timelapseIntervalMin", "").c_str());
  TEST_ASSERT_EQUAL_STRING("90", sent.get("retentionDays", "").c_str());
}

void test_blank_add_form_submits_the_defaults(void) {
  CameraConfig d;
  CameraConfig r = submit(fixtureAddDefault(), false);
  TEST_ASSERT_EQUAL(d.enabled, r.enabled);
  TEST_ASSERT_EQUAL(d.useWSSecurity, r.useWSSecurity);
  TEST_ASSERT_EQUAL(d.personAlertsEnabled, r.personAlertsEnabled);
  TEST_ASSERT_EQUAL(d.petAlertsEnabled, r.petAlertsEnabled);
  TEST_ASSERT_EQUAL_UINT32(d.alertCooldownMs, r.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(d.pollIntervalMs, r.pollIntervalMs);
  TEST_ASSERT_EQUAL_UINT16(0, r.snapshotMaxWidth);  // shown blank, reads back as unset
}

void test_no_field_name_is_used_twice(void) {
  BrowserSubmission b(renderCameraForm(fixtureEditHostile(), true));
  TEST_ASSERT_EQUAL(0, (int)b.duplicates().size());
}

void test_hostile_text_is_escaped(void) {
  String html = renderCameraForm(fixtureEditHostile(), true);
  TEST_ASSERT_EQUAL(-1, html.indexOf("<b>side</b>"));
  TEST_ASSERT_EQUAL(-1, html.indexOf("D01 <"));
  TEST_ASSERT_TRUE(html.indexOf("D01 &lt;&quot;Front&quot;") >= 0);
}

void test_edit_form_has_original_name_and_cancel_add_form_does_not(void) {
  String edit = renderCameraForm(fixtureEditHostile(), true);
  String add = renderCameraForm(fixtureEditHostile(), false);
  TEST_ASSERT_EQUAL_STRING(fixtureEditHostile().name.c_str(), BrowserSubmission(edit).get("originalName", "").c_str());
  TEST_ASSERT_FALSE(BrowserSubmission(add).has("originalName"));
  TEST_ASSERT_TRUE(edit.indexOf(">Cancel<") >= 0);
  TEST_ASSERT_EQUAL(-1, add.indexOf(">Cancel<"));
}

void test_labels_state_the_real_limits(void) {
  String html = renderCameraForm(fixtureAddDefault(), false);
  TEST_ASSERT_TRUE(html.indexOf("max " + String(CAMERA_ALERT_COOLDOWN_MAX_MS / 1000UL)) >= 0);
  TEST_ASSERT_TRUE(html.indexOf("max " + String(CAMERA_OFFLINE_THRESHOLD_MAX_MS / 60000UL)) >= 0);
  TEST_ASSERT_TRUE(html.indexOf("(1-" + String(CAMERA_SNAPSHOT_BURST_MAX) + ")") >= 0);
  TEST_ASSERT_TRUE(html.indexOf(String(CAMERA_POLL_INTERVAL_MIN_MS) + "-" + String(CAMERA_POLL_INTERVAL_MAX_MS)) >= 0);
}

void test_watchdog_warning_only_when_shorter_than_quiet_hours(void) {
  const char* warning = "shorter than or equal to the quiet hours window";
  TEST_ASSERT_TRUE(renderCameraForm(fixtureEditHostile(), true).indexOf(warning) >= 0);  // same-day window
  TEST_ASSERT_TRUE(renderCameraForm(fixtureWrapWarning(), true).indexOf(warning) >= 0);  // wraps midnight
  TEST_ASSERT_EQUAL(-1, renderCameraForm(fixtureWrapNoWarning(), true).indexOf(warning));
  TEST_ASSERT_EQUAL(-1, renderCameraForm(fixtureAddDefault(), false).indexOf(warning));
}

void test_render_never_echoes_password(void) {
  String html = renderCameraForm(fixtureEditHostile(), true);
  TEST_ASSERT_EQUAL(-1, html.indexOf("secret"));
}

// ---- Parsing ----

void test_parse_empty_form_uses_defaults(void) {
  MapParams p;
  CameraConfig c = parseCameraForm(p);
  CameraConfig d;
  TEST_ASSERT_EQUAL_STRING("", c.name.c_str());
  TEST_ASSERT_FALSE(c.enabled);  // unchecked boxes are simply absent
  TEST_ASSERT_FALSE(c.useWSSecurity);
  TEST_ASSERT_FALSE(c.personAlertsEnabled);
  TEST_ASSERT_EQUAL_UINT32(30000, c.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(5UL * 60000UL, c.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT(1, c.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT32(2000, c.pollIntervalMs);
  TEST_ASSERT_EQUAL_UINT16(0, c.quietStartMinute);
  TEST_ASSERT_EQUAL_UINT16(0, c.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT16(0, c.retentionDays);
  TEST_ASSERT_EQUAL_UINT16(d.snapshotMaxWidth, c.snapshotMaxWidth);
}

void test_parse_all_fields(void) {
  MapParams p;
  p.values = {{"name", "  Gate  "},
              {"deviceServiceUrl", "http://x/onvif"},
              {"user", "u"},
              {"pass", "p"},
              {"notes", "n"},
              {"snapshotUriOverride", "http://x/s"},
              {"preferredProfileKeyword", "sub"},
              {"enabled", "on"},
              {"useWSSecurity", "on"},
              {"includeInitialTerminationTime", "on"},
              {"includeReplyToAnonymous", "on"},
              {"quietHoursEnabled", "on"},
              {"timelapseSendToTelegram", "on"},
              {"personAlertsEnabled", "on"},
              {"vehicleAlertsEnabled", "on"},
              {"petAlertsEnabled", "on"},
              {"petAlertsTextOnly", "on"},
              {"motionDigestEnabled", "on"},
              {"alertCooldownSec", "45"},
              {"offlineThresholdMin", "10"},
              {"snapshotBurstCount", "3"},
              {"quietStart", "22:15"},
              {"quietEnd", "06:00"},
              {"motionWatchdogHours", "12"},
              {"timelapseIntervalMin", "30"},
              {"retentionDays", "60"},
              {"snapshotMaxWidth", "640"},
              {"snapshotMaxHeight", "480"},
              {"pollIntervalMs", "1500"}};
  CameraConfig c = parseCameraForm(p);
  TEST_ASSERT_EQUAL_STRING("Gate", c.name.c_str());  // trimmed
  TEST_ASSERT_EQUAL_STRING("http://x/onvif", c.deviceServiceUrl.c_str());
  TEST_ASSERT_EQUAL_STRING("u", c.user.c_str());
  TEST_ASSERT_EQUAL_STRING("p", c.pass.c_str());
  TEST_ASSERT_EQUAL_STRING("n", c.notes.c_str());
  TEST_ASSERT_EQUAL_STRING("http://x/s", c.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_STRING("sub", c.preferredProfileKeyword.c_str());
  TEST_ASSERT_TRUE(c.enabled);
  TEST_ASSERT_TRUE(c.useWSSecurity);
  TEST_ASSERT_TRUE(c.includeInitialTerminationTime);
  TEST_ASSERT_TRUE(c.includeReplyToAnonymous);
  TEST_ASSERT_TRUE(c.quietHoursEnabled);
  TEST_ASSERT_TRUE(c.timelapseSendToTelegram);
  TEST_ASSERT_TRUE(c.personAlertsEnabled);
  TEST_ASSERT_TRUE(c.vehicleAlertsEnabled);
  TEST_ASSERT_TRUE(c.petAlertsEnabled);
  TEST_ASSERT_TRUE(c.petAlertsTextOnly);
  TEST_ASSERT_TRUE(c.motionDigestEnabled);
  TEST_ASSERT_EQUAL_UINT32(45000, c.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(600000, c.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT(3, c.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT16(22 * 60 + 15, c.quietStartMinute);
  TEST_ASSERT_EQUAL_UINT16(6 * 60, c.quietEndMinute);
  TEST_ASSERT_EQUAL_UINT16(12, c.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT16(30, c.timelapseIntervalMin);
  TEST_ASSERT_EQUAL_UINT16(60, c.retentionDays);
  TEST_ASSERT_EQUAL_UINT16(640, c.snapshotMaxWidth);
  TEST_ASSERT_EQUAL_UINT16(480, c.snapshotMaxHeight);
  TEST_ASSERT_EQUAL_UINT32(1500, c.pollIntervalMs);
}

void test_parse_zero_or_negative_fall_back_to_defaults(void) {
  MapParams p;
  p.values = {{"alertCooldownSec", "0"}, {"offlineThresholdMin", "-3"}, {"snapshotBurstCount", "0"},
              {"pollIntervalMs", "0"}};
  CameraConfig c = parseCameraForm(p);
  CameraConfig d;
  TEST_ASSERT_EQUAL_UINT32(d.alertCooldownMs, c.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(d.offlineThresholdMs, c.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT(1, c.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT32(d.pollIntervalMs, c.pollIntervalMs);
}

void test_parse_clamps_upper_bounds(void) {
  MapParams p;
  p.values = {{"alertCooldownSec", "99999999"}, {"offlineThresholdMin", "99999999"},
              {"snapshotBurstCount", "500"},    {"motionWatchdogHours", "1000"},
              {"timelapseIntervalMin", "9999"}, {"retentionDays", "99999"},
              {"snapshotMaxWidth", "99999"},    {"snapshotMaxHeight", "99999"},
              {"pollIntervalMs", "99999999"}};
  CameraConfig c = parseCameraForm(p);
  TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_COOLDOWN_MAX_MS, c.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(CAMERA_OFFLINE_THRESHOLD_MAX_MS, c.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT(CAMERA_SNAPSHOT_BURST_MAX, c.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT16(168, c.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT16(1440, c.timelapseIntervalMin);
  TEST_ASSERT_EQUAL_UINT16(SD_RETENTION_MAX_DAYS, c.retentionDays);
  TEST_ASSERT_EQUAL_UINT16(CAMERA_SNAPSHOT_DIMENSION_MAX, c.snapshotMaxWidth);
  TEST_ASSERT_EQUAL_UINT16(CAMERA_SNAPSHOT_DIMENSION_MAX, c.snapshotMaxHeight);
  TEST_ASSERT_EQUAL_UINT32(CAMERA_POLL_INTERVAL_MAX_MS, c.pollIntervalMs);
}

void test_parse_clamps_lower_bounds(void) {
  MapParams p;
  p.values = {{"motionWatchdogHours", "-5"}, {"timelapseIntervalMin", "-5"}, {"retentionDays", "-5"},
              {"snapshotMaxWidth", "-5"},    {"snapshotMaxHeight", "-5"},    {"pollIntervalMs", "10"}};
  CameraConfig c = parseCameraForm(p);
  TEST_ASSERT_EQUAL_UINT16(0, c.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT16(0, c.timelapseIntervalMin);
  TEST_ASSERT_EQUAL_UINT16(0, c.retentionDays);
  TEST_ASSERT_EQUAL_UINT16(0, c.snapshotMaxWidth);
  TEST_ASSERT_EQUAL_UINT16(0, c.snapshotMaxHeight);
  TEST_ASSERT_EQUAL_UINT32(CAMERA_POLL_INTERVAL_MIN_MS, c.pollIntervalMs);  // raised, not discarded
}

void test_parse_malformed_quiet_times_are_midnight(void) {
  MapParams p;
  p.values = {{"quietStart", "7:30"}, {"quietEnd", "24:00"}};
  CameraConfig c = parseCameraForm(p);
  TEST_ASSERT_EQUAL_UINT16(0, c.quietStartMinute);
  TEST_ASSERT_EQUAL_UINT16(0, c.quietEndMinute);
}

// ---- HH:MM helpers ----

void test_hhmm_round_trip(void) {
  TEST_ASSERT_EQUAL_STRING("00:00", minutesToHHMM(0).c_str());
  TEST_ASSERT_EQUAL_STRING("23:59", minutesToHHMM(1439).c_str());
  TEST_ASSERT_EQUAL_UINT16(1439, parseHHMMToMinutes("23:59"));
  TEST_ASSERT_EQUAL_UINT16(0, parseHHMMToMinutes("ab:cd"));
  TEST_ASSERT_EQUAL_UINT16(0, parseHHMMToMinutes("12:60"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_keeps_every_field);
  RUN_TEST(test_each_checkbox_name_controls_its_own_setting);
  RUN_TEST(test_each_value_field_name_shows_its_own_setting);
  RUN_TEST(test_blank_add_form_submits_the_defaults);
  RUN_TEST(test_no_field_name_is_used_twice);
  RUN_TEST(test_hostile_text_is_escaped);
  RUN_TEST(test_edit_form_has_original_name_and_cancel_add_form_does_not);
  RUN_TEST(test_labels_state_the_real_limits);
  RUN_TEST(test_watchdog_warning_only_when_shorter_than_quiet_hours);
  RUN_TEST(test_render_never_echoes_password);
  RUN_TEST(test_parse_empty_form_uses_defaults);
  RUN_TEST(test_parse_all_fields);
  RUN_TEST(test_parse_zero_or_negative_fall_back_to_defaults);
  RUN_TEST(test_parse_clamps_upper_bounds);
  RUN_TEST(test_parse_clamps_lower_bounds);
  RUN_TEST(test_parse_malformed_quiet_times_are_midnight);
  RUN_TEST(test_hhmm_round_trip);
  return UNITY_END();
}
