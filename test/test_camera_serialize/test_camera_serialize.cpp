#include <unity.h>
#include <Arduino.h>
#include "camera_serialize.h"

void setUp(void) {}
void tearDown(void) {}

// The real field separator (camera_serialize.cpp's private FIELD_SEP) is
// non-printable ASCII 0x1F, not '|' - joins fields with the real
// character so the "legacy record" tests below actually exercise the real
// format instead of accidentally producing one giant unparseable field.
static String joinFields(std::initializer_list<const char*> fields) {
  String out;
  bool first = true;
  for (const char* f : fields) {
    if (!first) out += (char)0x1F;
    out += f;
    first = false;
  }
  return out;
}

static CameraConfig sampleCamera() {
  CameraConfig c;
  c.name = "D01-FrontDoor";
  c.deviceServiceUrl = "http://192.168.1.50/onvif/device_service";
  c.enabled = true;
  c.useWSSecurity = false;
  c.includeInitialTerminationTime = true;
  c.includeReplyToAnonymous = false;
  c.snapshotUriOverride = "http://192.168.1.50/snap.jpg?u={USER}&p={PASS}";
  c.preferredProfileKeyword = "sub";
  c.user = "admin";
  c.pass = "s3cret!";
  c.notes = "flaky wifi, retry twice";
  c.alertCooldownMs = 45000;
  c.offlineThresholdMs = 120000;
  c.snapshotBurstCount = 3;
  c.quietHoursEnabled = true;
  c.quietStartMinute = 1320; // 22:00
  c.quietEndMinute = 360;    // 06:00
  c.motionWatchdogHours = 24;
  c.timelapseIntervalMin = 15;
  c.pollIntervalMs = 1000;
  c.timelapseSendToTelegram = true;
  c.retentionDays = 45;
  c.petAlertsEnabled = true;
  c.petAlertsTextOnly = true;
  c.snapshotMaxWidth = 800;
  c.snapshotMaxHeight = 600;
  // Both false - the non-default value (CameraConfig's own default is
  // true for both) so the round trip below actually exercises a real
  // change, not just "did the default survive."
  c.personAlertsEnabled = false;
  c.vehicleAlertsEnabled = false;
  c.motionDigestEnabled = false; // same reasoning - CameraConfig's own default is true
  return c;
}

// A full round trip must reproduce every field exactly - this is the
// "silent NVS corruption" risk directly: if a future field gets inserted
// in the middle of the list instead of appended at the end, this is the
// test that should fail instead of every camera's saved settings silently
// shifting into the wrong fields on the next boot. serializeCamera()
// always writes CAMERA_SCHEMA_VERSION's layout, so that's what it must be
// read back as.
void test_round_trip_preserves_every_field(void) {
  CameraConfig original = sampleCamera();
  CameraConfig restored = deserializeCamera(serializeCamera(original), CAMERA_SCHEMA_VERSION);

  TEST_ASSERT_EQUAL_STRING(original.name.c_str(), restored.name.c_str());
  TEST_ASSERT_EQUAL_STRING(original.deviceServiceUrl.c_str(), restored.deviceServiceUrl.c_str());
  TEST_ASSERT_EQUAL(original.enabled, restored.enabled);
  TEST_ASSERT_EQUAL(original.useWSSecurity, restored.useWSSecurity);
  TEST_ASSERT_EQUAL(original.includeInitialTerminationTime, restored.includeInitialTerminationTime);
  TEST_ASSERT_EQUAL(original.includeReplyToAnonymous, restored.includeReplyToAnonymous);
  TEST_ASSERT_EQUAL_STRING(original.snapshotUriOverride.c_str(), restored.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_STRING(original.preferredProfileKeyword.c_str(), restored.preferredProfileKeyword.c_str());
  TEST_ASSERT_EQUAL_STRING(original.user.c_str(), restored.user.c_str());
  TEST_ASSERT_EQUAL_STRING(original.pass.c_str(), restored.pass.c_str());
  TEST_ASSERT_EQUAL_STRING(original.notes.c_str(), restored.notes.c_str());
  TEST_ASSERT_EQUAL_UINT32(original.alertCooldownMs, restored.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(original.offlineThresholdMs, restored.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(original.snapshotBurstCount, restored.snapshotBurstCount);
  TEST_ASSERT_EQUAL(original.quietHoursEnabled, restored.quietHoursEnabled);
  TEST_ASSERT_EQUAL_UINT32(original.quietStartMinute, restored.quietStartMinute);
  TEST_ASSERT_EQUAL_UINT32(original.quietEndMinute, restored.quietEndMinute);
  TEST_ASSERT_EQUAL_UINT32(original.motionWatchdogHours, restored.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT32(original.timelapseIntervalMin, restored.timelapseIntervalMin);
  TEST_ASSERT_EQUAL_UINT32(original.pollIntervalMs, restored.pollIntervalMs);
  TEST_ASSERT_EQUAL(original.timelapseSendToTelegram, restored.timelapseSendToTelegram);
  TEST_ASSERT_EQUAL_UINT32(original.retentionDays, restored.retentionDays);
  TEST_ASSERT_EQUAL(original.petAlertsEnabled, restored.petAlertsEnabled);
  TEST_ASSERT_EQUAL(original.petAlertsTextOnly, restored.petAlertsTextOnly);
  TEST_ASSERT_EQUAL_UINT32(original.snapshotMaxWidth, restored.snapshotMaxWidth);
  TEST_ASSERT_EQUAL_UINT32(original.snapshotMaxHeight, restored.snapshotMaxHeight);
  TEST_ASSERT_EQUAL(original.personAlertsEnabled, restored.personAlertsEnabled);
  TEST_ASSERT_EQUAL(original.vehicleAlertsEnabled, restored.vehicleAlertsEnabled);
  TEST_ASSERT_EQUAL(original.motionDigestEnabled, restored.motionDigestEnabled);
}

// personAlertsEnabled/vehicleAlertsEnabled/motionDigestEnabled all default
// to true (opt-OUT, unlike every other appended bool in this file which
// defaults to false) - a freshly-constructed camera (never touched these
// settings at all) must still round-trip as true, not silently flip to
// false the way an ordinary appended bool's "empty field" handling would.
void test_round_trip_person_vehicle_alerts_default_true(void) {
  CameraConfig c;
  c.name = "D03";
  c.deviceServiceUrl = "http://192.168.1.52/onvif/device_service";
  CameraConfig restored = deserializeCamera(serializeCamera(c), CAMERA_SCHEMA_VERSION);
  TEST_ASSERT_TRUE(restored.personAlertsEnabled);
  TEST_ASSERT_TRUE(restored.vehicleAlertsEnabled);
  TEST_ASSERT_TRUE(restored.motionDigestEnabled);
}

void test_round_trip_with_falsy_flags_and_empty_optionals(void) {
  CameraConfig c;
  c.name = "D02";
  c.deviceServiceUrl = "http://192.168.1.51/onvif/device_service";
  c.enabled = false;
  c.useWSSecurity = false;
  c.includeInitialTerminationTime = false;
  c.includeReplyToAnonymous = false;
  // snapshotUriOverride, preferredProfileKeyword, user, pass, notes left empty.

  CameraConfig restored = deserializeCamera(serializeCamera(c), CAMERA_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("D02", restored.name.c_str());
  TEST_ASSERT_FALSE(restored.enabled);
  TEST_ASSERT_FALSE(restored.useWSSecurity);
  TEST_ASSERT_EQUAL_STRING("", restored.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_STRING("", restored.user.c_str());
}

// deserializeCamera's caller (camera_store.cpp's loadCameras()) treats an
// empty name as "malformed, skip this entry (and log it)" - so malformed
// input must come back with an empty name, not a partially-populated
// garbage record, for both the legacy and current-version parsers.
void test_malformed_record_returns_empty_name_v0(void) {
  CameraConfig c = deserializeCamera("only|three|fields", 0);
  TEST_ASSERT_EQUAL_STRING("", c.name.c_str());
}

void test_malformed_record_returns_empty_name_current_version(void) {
  CameraConfig c = deserializeCamera("only|three|fields", CAMERA_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("", c.name.c_str());
}

void test_empty_record_returns_empty_name(void) {
  CameraConfig c = deserializeCamera("", 0);
  TEST_ASSERT_EQUAL_STRING("", c.name.c_str());
}

// ---- Version 0 (pre-versioning, field-count-tolerant) ----

// Records saved before alertCooldownMs/offlineThresholdMs/snapshotBurstCount
// existed have only the original 11 fields - loading one (as version 0)
// must fall back to CameraConfig's own defaults for the missing fields,
// not zero them out (a 0ms cooldown would mean "alert on every single poll").
void test_v0_11_field_record_gets_current_defaults(void) {
  CameraConfig fresh; // for the defaults to compare against
  String legacy = joinFields({"D03", "http://192.168.1.52/onvif/device_service", "1", "1", "0", "0",
                               "", "", "user", "pass", "notes"});
  CameraConfig restored = deserializeCamera(legacy, 0);

  TEST_ASSERT_EQUAL_STRING("D03", restored.name.c_str());
  TEST_ASSERT_EQUAL_UINT32(fresh.alertCooldownMs, restored.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(fresh.offlineThresholdMs, restored.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(fresh.snapshotBurstCount, restored.snapshotBurstCount);
}

// Records saved after alertCooldownMs/offlineThresholdMs were added but
// before snapshotBurstCount existed (13 fields) - same backward-compat
// reasoning, just for the newer field only.
void test_v0_13_field_record_defaults_only_snapshotBurstCount(void) {
  CameraConfig fresh;
  String legacy = joinFields({"D04", "http://192.168.1.53/onvif/device_service", "1", "1", "0", "0",
                               "", "", "user", "pass", "notes", "60000", "300000"});
  CameraConfig restored = deserializeCamera(legacy, 0);

  TEST_ASSERT_EQUAL_UINT32(60000, restored.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(300000, restored.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(fresh.snapshotBurstCount, restored.snapshotBurstCount);
}

// The boundary test_v0_11/_13_field tests above skip: exactly 12 fields
// (alertCooldownMs present, offlineThresholdMs/snapshotBurstCount not) - a
// `>= 12` -> `> 12` mutation in deserializeCameraV0 would pass both of
// those existing tests unchanged but silently lose a real 12-field legacy
// record's alertCooldownMs.
void test_v0_12_field_record_keeps_cooldown_defaults_the_rest(void) {
  CameraConfig fresh;
  String legacy = joinFields({"D08", "http://192.168.1.57/onvif/device_service", "1", "1", "0", "0",
                               "", "", "user", "pass", "notes", "60000"});
  CameraConfig restored = deserializeCamera(legacy, 0);

  TEST_ASSERT_EQUAL_UINT32(60000, restored.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(fresh.offlineThresholdMs, restored.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(fresh.snapshotBurstCount, restored.snapshotBurstCount);
}

// One record with every field different from its default, so a field read
// from the wrong column, or not at all, shows up.
static const char* const kAllFields[] = {
    "D07", "http://x/onvif", "0", "0", "1", "1", "snap", "kw", "usr", "pw", "nt", "111", "222", "3", "1", "44", "55",
    "66", "77", "888", "1", "99", "1", "1", "640", "360", "0", "0", "0"};
static const size_t kAllFieldCount = sizeof(kAllFields) / sizeof(kAllFields[0]);

static String firstFields(size_t count) {
  String out;
  for (size_t i = 0; i < count; i++) {
    if (i > 0) out += (char)0x1F;
    out += i < kAllFieldCount ? kAllFields[i] : "extra";
  }
  return out;
}

static std::vector<String> columns(const String& record) {
  std::vector<String> out;
  int start = 0;
  for (int i = 0; i <= (int)record.length(); i++) {
    if (i == (int)record.length() || record[i] == (char)0x1F) { out.push_back(record.substring(start, i)); start = i + 1; }
  }
  return out;
}

void test_every_version_reads_exactly_its_own_fields(void) {
  struct { uint16_t version; size_t count; } kLayouts[] = {{1, 14}, {2, 19}, {3, 20}, {4, 22}, {5, 24}, {6, 26}, {7, 28}, {8, 29}};
  std::vector<String> defaults = columns(serializeCamera(CameraConfig()));
  TEST_ASSERT_EQUAL((int)kAllFieldCount, (int)defaults.size());

  for (auto& layout : kLayouts) {
    char label[32];
    snprintf(label, sizeof(label), "version %u", (unsigned)layout.version);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", deserializeCamera(firstFields(layout.count - 1), layout.version).name.c_str(), label);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", deserializeCamera(firstFields(layout.count + 1), layout.version).name.c_str(), label);

    std::vector<String> got = columns(serializeCamera(deserializeCamera(firstFields(layout.count), layout.version)));
    for (size_t i = 0; i < kAllFieldCount; i++) {
      const char* expected = i < layout.count ? kAllFields[i] : defaults[i].c_str();
      TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, got[i].c_str(), label);
    }
  }
}

// A version newer than this build knows about (firmware downgraded after
// a later version changed the layout) falls through to the newest known
// (V8) layout rather than being discarded outright - camera_store.cpp is
// responsible for logging a warning when this happens, so this test only
// covers that it doesn't crash and still extracts something.
void test_unknown_future_version_falls_back_to_newest_known_layout(void) {
  String record = joinFields({"D06", "http://192.168.1.55/onvif/device_service", "1", "1", "0", "0",
                               "", "", "user", "pass", "notes", "60000", "300000", "5",
                               "0", "0", "0", "0", "0", "1500", "1", "45", "1", "1", "800", "600",
                               "0", "0", "0"}); // 29 fields
  CameraConfig restored = deserializeCamera(record, (uint16_t)(CAMERA_SCHEMA_VERSION + 1));
  TEST_ASSERT_EQUAL_STRING("D06", restored.name.c_str());
  TEST_ASSERT_EQUAL_UINT32(5, restored.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT32(1500, restored.pollIntervalMs);
  TEST_ASSERT_TRUE(restored.timelapseSendToTelegram);
  TEST_ASSERT_EQUAL_UINT32(45, restored.retentionDays);
  TEST_ASSERT_TRUE(restored.petAlertsEnabled);
  TEST_ASSERT_TRUE(restored.petAlertsTextOnly);
  TEST_ASSERT_EQUAL_UINT32(800, restored.snapshotMaxWidth);
  TEST_ASSERT_EQUAL_UINT32(600, restored.snapshotMaxHeight);
  TEST_ASSERT_FALSE(restored.personAlertsEnabled);
  TEST_ASSERT_FALSE(restored.vehicleAlertsEnabled);
  TEST_ASSERT_FALSE(restored.motionDigestEnabled);
}

// A name/note containing the field separator character must not corrupt
// the record's field count - stripSeparators() is what's responsible for
// this, exercised indirectly through serializeCamera().
void test_field_separator_character_in_input_is_stripped_not_corrupting(void) {
  CameraConfig c = sampleCamera();
  c.notes = String("line one") + (char)0x1F + "line two"; // 0x1F is FIELD_SEP
  CameraConfig restored = deserializeCamera(serializeCamera(c), CAMERA_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING(c.name.c_str(), restored.name.c_str()); // fields still line up
  TEST_ASSERT_EQUAL_STRING("line oneline two", restored.notes.c_str());
}

// ---- sortCamerasByName ----

static CameraConfig camWithName(const char* name) {
  CameraConfig c;
  c.name = name;
  return c;
}

void test_sortCamerasByName_orders_alphabetically(void) {
  std::vector<CameraConfig> cams = {camWithName("D02-FEsq"), camWithName("D01-FDir")};
  sortCamerasByName(cams);
  TEST_ASSERT_EQUAL_STRING("D01-FDir", cams[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("D02-FEsq", cams[1].name.c_str());
}

void test_sortCamerasByName_is_case_insensitive(void) {
  std::vector<CameraConfig> cams = {camWithName("banana"), camWithName("Apple")};
  sortCamerasByName(cams);
  TEST_ASSERT_EQUAL_STRING("Apple", cams[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("banana", cams[1].name.c_str());
}

// The exact real-world case this was added for: zero-padded numeric
// suffixes sort correctly as plain text, without needing a natural/
// numeric-aware comparator - "D10" must land after "D09", not before it
// (and not before "D01" just because it was inserted first).
void test_sortCamerasByName_zero_padded_numeric_suffixes_in_order(void) {
  std::vector<CameraConfig> cams = {camWithName("D10-2Lentes"), camWithName("D02-FEsq"),
                                     camWithName("D01-FDir"), camWithName("D09-Portao")};
  sortCamerasByName(cams);
  TEST_ASSERT_EQUAL_STRING("D01-FDir", cams[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("D02-FEsq", cams[1].name.c_str());
  TEST_ASSERT_EQUAL_STRING("D09-Portao", cams[2].name.c_str());
  TEST_ASSERT_EQUAL_STRING("D10-2Lentes", cams[3].name.c_str());
}

// The stored layout itself: every position, in order. A row inserted or moved
// in the table fails here, and would otherwise misread every saved camera.
void test_stored_layout_is_pinned(void) {
  CameraConfig c;
  c.name = "n"; c.deviceServiceUrl = "u"; c.enabled = true; c.useWSSecurity = false;
  c.includeInitialTerminationTime = true; c.includeReplyToAnonymous = false;
  c.snapshotUriOverride = "snap"; c.preferredProfileKeyword = "kw"; c.user = "usr"; c.pass = "pw"; c.notes = "nt";
  c.alertCooldownMs = 111; c.offlineThresholdMs = 222; c.snapshotBurstCount = 3;
  c.quietHoursEnabled = true; c.quietStartMinute = 44; c.quietEndMinute = 55;
  c.motionWatchdogHours = 66; c.timelapseIntervalMin = 77; c.pollIntervalMs = 888;
  c.timelapseSendToTelegram = true; c.retentionDays = 99;
  c.petAlertsEnabled = true; c.petAlertsTextOnly = false;
  c.snapshotMaxWidth = 640; c.snapshotMaxHeight = 360;
  c.personAlertsEnabled = false; c.vehicleAlertsEnabled = true; c.motionDigestEnabled = false;

  String expected = joinFields({"n", "u", "1", "0", "1", "0", "snap", "kw", "usr", "pw", "nt", "111", "222", "3",
                                "1", "44", "55", "66", "77", "888", "1", "99", "1", "0", "640", "360", "0", "1", "0"});
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), serializeCamera(c).c_str());
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), serializeCamera(deserializeCamera(expected, CAMERA_SCHEMA_VERSION)).c_str());
}

void test_current_layout_matches_the_schema_version(void) {
  // A row added without bumping CAMERA_SCHEMA_VERSION would write records this
  // build then refuses to read.
  TEST_ASSERT_EQUAL_STRING(String((unsigned)cameraRecordFieldCount(serializeCamera(CameraConfig()))).c_str(),
                           cameraRecordExpectedFields(CAMERA_SCHEMA_VERSION).c_str());
}

void test_empty_alert_flags_keep_their_default_on(void) {
  String record = joinFields({"n", "u", "1", "1", "0", "0", "", "", "usr", "pw", "", "", "", "", "0", "", "", "", "",
                              "", "0", "", "0", "0", "", "", "", "", ""});
  CameraConfig c = deserializeCamera(record, CAMERA_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("n", c.name.c_str());
  TEST_ASSERT_TRUE(c.personAlertsEnabled);
  TEST_ASSERT_TRUE(c.vehicleAlertsEnabled);
  TEST_ASSERT_TRUE(c.motionDigestEnabled);
  TEST_ASSERT_EQUAL_UINT32(CameraConfig().alertCooldownMs, c.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(CameraConfig().pollIntervalMs, c.pollIntervalMs);
}

void test_expected_field_counts_per_version(void) {
  static const char* const kExpected[] = {"11-14", "14", "19", "20", "22", "24", "26", "28", "29"};
  for (uint16_t v = 0; v < sizeof(kExpected) / sizeof(kExpected[0]); v++) {
    TEST_ASSERT_EQUAL_STRING(kExpected[v], cameraRecordExpectedFields(v).c_str());
  }
  TEST_ASSERT_EQUAL_STRING("29", cameraRecordExpectedFields(CAMERA_SCHEMA_VERSION + 5).c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_every_version_reads_exactly_its_own_fields);
  RUN_TEST(test_stored_layout_is_pinned);
  RUN_TEST(test_empty_alert_flags_keep_their_default_on);
  RUN_TEST(test_current_layout_matches_the_schema_version);
  RUN_TEST(test_expected_field_counts_per_version);
  RUN_TEST(test_round_trip_preserves_every_field);
  RUN_TEST(test_round_trip_person_vehicle_alerts_default_true);
  RUN_TEST(test_round_trip_with_falsy_flags_and_empty_optionals);
  RUN_TEST(test_malformed_record_returns_empty_name_v0);
  RUN_TEST(test_malformed_record_returns_empty_name_current_version);
  RUN_TEST(test_empty_record_returns_empty_name);
  RUN_TEST(test_v0_11_field_record_gets_current_defaults);
  RUN_TEST(test_v0_13_field_record_defaults_only_snapshotBurstCount);
  RUN_TEST(test_v0_12_field_record_keeps_cooldown_defaults_the_rest);
  RUN_TEST(test_unknown_future_version_falls_back_to_newest_known_layout);
  RUN_TEST(test_field_separator_character_in_input_is_stripped_not_corrupting);
  RUN_TEST(test_sortCamerasByName_orders_alphabetically);
  RUN_TEST(test_sortCamerasByName_is_case_insensitive);
  RUN_TEST(test_sortCamerasByName_zero_padded_numeric_suffixes_in_order);
  return UNITY_END();
}
