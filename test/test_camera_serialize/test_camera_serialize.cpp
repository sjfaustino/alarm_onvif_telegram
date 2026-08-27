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
// shifting into the wrong fields on the next boot.
void test_round_trip_preserves_every_field(void) {
  CameraConfig original = sampleCamera();
  CameraConfig restored = deserializeCamera(serializeCamera(original));

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

  CameraConfig restored = deserializeCamera(serializeCamera(c));
  TEST_ASSERT_EQUAL_STRING("D02", restored.name.c_str());
  TEST_ASSERT_FALSE(restored.enabled);
  TEST_ASSERT_FALSE(restored.useWSSecurity);
  TEST_ASSERT_EQUAL_STRING("", restored.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_STRING("", restored.user.c_str());
}

// deserializeCamera's caller (camera_store.cpp's loadCameras()) treats an
// empty name as "malformed, skip this entry" - so malformed input must
// come back with an empty name, not a partially-populated garbage record.
void test_malformed_record_returns_empty_name(void) {
  CameraConfig c = deserializeCamera("only|three|fields");
  TEST_ASSERT_EQUAL_STRING("", c.name.c_str());
}

void test_empty_record_returns_empty_name(void) {
  CameraConfig c = deserializeCamera("");
  TEST_ASSERT_EQUAL_STRING("", c.name.c_str());
}

// Records saved before alertCooldownMs/offlineThresholdMs/snapshotBurstCount
// existed have only the original 11 fields - loading one must fall back to
// CameraConfig's own defaults for the missing fields, not zero them out
// (a 0ms cooldown would mean "alert on every single poll").
void test_legacy_11_field_record_gets_current_defaults(void) {
  CameraConfig fresh; // for the defaults to compare against
  String legacy = joinFields({"D03", "http://192.168.1.52/onvif/device_service", "1", "1", "0", "0",
                               "", "", "user", "pass", "notes"});
  CameraConfig restored = deserializeCamera(legacy);

  TEST_ASSERT_EQUAL_STRING("D03", restored.name.c_str());
  TEST_ASSERT_EQUAL_UINT32(fresh.alertCooldownMs, restored.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(fresh.offlineThresholdMs, restored.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(fresh.snapshotBurstCount, restored.snapshotBurstCount);
}

// Records saved after alertCooldownMs/offlineThresholdMs were added but
// before snapshotBurstCount existed (13 fields) - same backward-compat
// reasoning, just for the newer field only.
void test_legacy_13_field_record_defaults_only_snapshotBurstCount(void) {
  CameraConfig fresh;
  String legacy = joinFields({"D04", "http://192.168.1.53/onvif/device_service", "1", "1", "0", "0",
                               "", "", "user", "pass", "notes", "60000", "300000"});
  CameraConfig restored = deserializeCamera(legacy);

  TEST_ASSERT_EQUAL_UINT32(60000, restored.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(300000, restored.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(fresh.snapshotBurstCount, restored.snapshotBurstCount);
}

// A name/note containing the field separator character must not corrupt
// the record's field count - stripSeparators() is what's responsible for
// this, exercised indirectly through serializeCamera().
void test_field_separator_character_in_input_is_stripped_not_corrupting(void) {
  CameraConfig c = sampleCamera();
  c.notes = String("line one") + (char)0x1F + "line two"; // 0x1F is FIELD_SEP
  CameraConfig restored = deserializeCamera(serializeCamera(c));
  TEST_ASSERT_EQUAL_STRING(c.name.c_str(), restored.name.c_str()); // fields still line up
  TEST_ASSERT_EQUAL_STRING("line oneline two", restored.notes.c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_preserves_every_field);
  RUN_TEST(test_round_trip_with_falsy_flags_and_empty_optionals);
  RUN_TEST(test_malformed_record_returns_empty_name);
  RUN_TEST(test_empty_record_returns_empty_name);
  RUN_TEST(test_legacy_11_field_record_gets_current_defaults);
  RUN_TEST(test_legacy_13_field_record_defaults_only_snapshotBurstCount);
  RUN_TEST(test_field_separator_character_in_input_is_stripped_not_corrupting);
  return UNITY_END();
}
