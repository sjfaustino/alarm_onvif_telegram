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

// ---- Version CAMERA_SCHEMA_VERSION (current, strict) ----

// This is the actual fix: unlike version 0, a record tagged as the
// *current* schema version must have exactly the current field count.
// Before schema versioning existed, this same 13-field record would have
// been silently accepted and misparsed as "missing snapshotBurstCount"
// (which happened to be correct by luck, since the only historical field
// changes were pure appends) - now that acceptance is deliberate and
// scoped to version 0 only. A record explicitly tagged as the current
// version with the wrong count is corruption, not "an older save".
void test_v1_wrong_field_count_is_rejected_not_reinterpreted(void) {
  String wrongCount = joinFields({"D05", "http://192.168.1.54/onvif/device_service", "1", "1", "0", "0",
                                   "", "", "user", "pass", "notes", "60000", "300000"}); // 13 fields
  CameraConfig restored = deserializeCamera(wrongCount, CAMERA_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("", restored.name.c_str());
}

void test_v1_exact_field_count_is_accepted(void) {
  String exact = joinFields({"D05", "http://192.168.1.54/onvif/device_service", "1", "1", "0", "0",
                              "", "", "user", "pass", "notes", "60000", "300000", "2"}); // 14 fields
  CameraConfig restored = deserializeCamera(exact, CAMERA_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("D05", restored.name.c_str());
  TEST_ASSERT_EQUAL_UINT32(2, restored.snapshotBurstCount);
}

// A version newer than this build knows about (firmware downgraded after
// a later version changed the layout) falls through to the newest known
// layout rather than being discarded outright - camera_store.cpp is
// responsible for logging a warning when this happens, so this test only
// covers that it doesn't crash and still extracts something.
void test_unknown_future_version_falls_back_to_newest_known_layout(void) {
  String record = joinFields({"D06", "http://192.168.1.55/onvif/device_service", "1", "1", "0", "0",
                               "", "", "user", "pass", "notes", "60000", "300000", "5"});
  CameraConfig restored = deserializeCamera(record, (uint16_t)(CAMERA_SCHEMA_VERSION + 1));
  TEST_ASSERT_EQUAL_STRING("D06", restored.name.c_str());
  TEST_ASSERT_EQUAL_UINT32(5, restored.snapshotBurstCount);
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

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_preserves_every_field);
  RUN_TEST(test_round_trip_with_falsy_flags_and_empty_optionals);
  RUN_TEST(test_malformed_record_returns_empty_name_v0);
  RUN_TEST(test_malformed_record_returns_empty_name_current_version);
  RUN_TEST(test_empty_record_returns_empty_name);
  RUN_TEST(test_v0_11_field_record_gets_current_defaults);
  RUN_TEST(test_v0_13_field_record_defaults_only_snapshotBurstCount);
  RUN_TEST(test_v1_wrong_field_count_is_rejected_not_reinterpreted);
  RUN_TEST(test_v1_exact_field_count_is_accepted);
  RUN_TEST(test_unknown_future_version_falls_back_to_newest_known_layout);
  RUN_TEST(test_field_separator_character_in_input_is_stripped_not_corrupting);
  RUN_TEST(test_sortCamerasByName_orders_alphabetically);
  RUN_TEST(test_sortCamerasByName_is_case_insensitive);
  RUN_TEST(test_sortCamerasByName_zero_padded_numeric_suffixes_in_order);
  return UNITY_END();
}
