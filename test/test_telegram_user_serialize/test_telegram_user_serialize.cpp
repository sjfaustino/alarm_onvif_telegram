#include <unity.h>
#include <Arduino.h>
#include "telegram_user_serialize.h"

void setUp(void) {}
void tearDown(void) {}

static String joinFields(std::initializer_list<const char*> fields) {
  String out;
  bool first = true;
  for (const char* f : fields) {
    if (!first) out += (char)0x1F; // real FIELD_SEP - see camera_serialize tests for why
    out += f;
    first = false;
  }
  return out;
}

static TelegramUser sampleUser() {
  TelegramUser u;
  u.name = "Admin";
  u.chatId = "123456789";
  u.allCameras = false;
  u.cameraNames = {"D01-FrontDoor", "D02-BackGate"};
  u.systemMessages = true;
  u.canCommand = true;
  u.canSnap = false;
  return u;
}

// Round trip must preserve every field, including the whole cameraNames
// list - same "silent NVS corruption" concern as camera_serialize's tests.
// serializeUser() always writes TELEGRAM_USER_SCHEMA_VERSION's layout, so
// that's what it must be read back as.
void test_round_trip_preserves_every_field(void) {
  TelegramUser original = sampleUser();
  TelegramUser restored = deserializeUser(serializeUser(original), TELEGRAM_USER_SCHEMA_VERSION);

  TEST_ASSERT_EQUAL_STRING(original.name.c_str(), restored.name.c_str());
  TEST_ASSERT_EQUAL_STRING(original.chatId.c_str(), restored.chatId.c_str());
  TEST_ASSERT_EQUAL(original.allCameras, restored.allCameras);
  TEST_ASSERT_EQUAL(original.systemMessages, restored.systemMessages);
  TEST_ASSERT_EQUAL(original.canCommand, restored.canCommand);
  TEST_ASSERT_EQUAL(original.canSnap, restored.canSnap);
  TEST_ASSERT_EQUAL_INT(2, (int)restored.cameraNames.size());
  TEST_ASSERT_EQUAL_STRING("D01-FrontDoor", restored.cameraNames[0].c_str());
  TEST_ASSERT_EQUAL_STRING("D02-BackGate", restored.cameraNames[1].c_str());
}

void test_round_trip_allCameras_true_with_empty_camera_list(void) {
  TelegramUser u;
  u.name = "Everyone";
  u.chatId = "1";
  u.allCameras = true;
  // cameraNames deliberately left empty - the real UI ignores it when
  // allCameras is checked, but the serialized form should still round-trip.
  TelegramUser restored = deserializeUser(serializeUser(u), TELEGRAM_USER_SCHEMA_VERSION);
  TEST_ASSERT_TRUE(restored.allCameras);
  TEST_ASSERT_EQUAL_INT(0, (int)restored.cameraNames.size());
}

// deserializeUser's caller (loadTelegramUsers()) treats an empty name as
// "malformed, skip this entry (and log it)".
void test_malformed_record_returns_empty_name_v0(void) {
  TelegramUser u = deserializeUser("only|two", 0);
  TEST_ASSERT_EQUAL_STRING("", u.name.c_str());
}

void test_malformed_record_returns_empty_name_current_version(void) {
  TelegramUser u = deserializeUser("only|two", TELEGRAM_USER_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("", u.name.c_str());
}

// ---- Version 0 (pre-versioning, field-count-tolerant) ----

// Records saved before canSnap existed have only the original 6 fields -
// loading one (as version 0) must default canSnap to false (TelegramUser's
// own default), not crash or misparse the rest.
void test_v0_6_field_record_defaults_canSnap_false(void) {
  String legacy = joinFields({"OldUser", "42", "1", "", "1", "1"});
  TelegramUser restored = deserializeUser(legacy, 0);

  TEST_ASSERT_EQUAL_STRING("OldUser", restored.name.c_str());
  TEST_ASSERT_TRUE(restored.canCommand);
  TEST_ASSERT_FALSE(restored.canSnap);
}

// ---- Version TELEGRAM_USER_SCHEMA_VERSION (current, strict) ----

// The actual fix: a record tagged as the *current* schema version must
// have exactly the current field count - a 6-field record explicitly
// tagged as the current version is corruption, not "an older save" (that
// interpretation is scoped to version 0 only now). See
// test_camera_serialize's equivalent test for the full rationale.
void test_v1_wrong_field_count_is_rejected_not_reinterpreted(void) {
  String wrongCount = joinFields({"User", "42", "1", "", "1", "1"}); // 6 fields
  TelegramUser restored = deserializeUser(wrongCount, TELEGRAM_USER_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("", restored.name.c_str());
}

void test_v1_exact_field_count_is_accepted(void) {
  String exact = joinFields({"User", "42", "1", "", "1", "1", "0"}); // 7 fields
  TelegramUser restored = deserializeUser(exact, TELEGRAM_USER_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("User", restored.name.c_str());
  TEST_ASSERT_FALSE(restored.canSnap);
}

void test_unknown_future_version_falls_back_to_newest_known_layout(void) {
  String record = joinFields({"User", "42", "1", "", "1", "1", "1"});
  TelegramUser restored = deserializeUser(record, (uint16_t)(TELEGRAM_USER_SCHEMA_VERSION + 1));
  TEST_ASSERT_EQUAL_STRING("User", restored.name.c_str());
  TEST_ASSERT_TRUE(restored.canSnap);
}

// ---- telegramUserWantsCamera ----

void test_wantsCamera_true_when_allCameras(void) {
  TelegramUser u;
  u.allCameras = true;
  TEST_ASSERT_TRUE(telegramUserWantsCamera(u, "AnyCameraAtAll"));
}

void test_wantsCamera_true_for_listed_camera_case_insensitive(void) {
  TelegramUser u;
  u.allCameras = false;
  u.cameraNames = {"D01-FrontDoor"};
  TEST_ASSERT_TRUE(telegramUserWantsCamera(u, "d01-frontdoor"));
}

void test_wantsCamera_false_for_unlisted_camera(void) {
  TelegramUser u;
  u.allCameras = false;
  u.cameraNames = {"D01-FrontDoor"};
  TEST_ASSERT_FALSE(telegramUserWantsCamera(u, "D02-BackGate"));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_preserves_every_field);
  RUN_TEST(test_round_trip_allCameras_true_with_empty_camera_list);
  RUN_TEST(test_malformed_record_returns_empty_name_v0);
  RUN_TEST(test_malformed_record_returns_empty_name_current_version);
  RUN_TEST(test_v0_6_field_record_defaults_canSnap_false);
  RUN_TEST(test_v1_wrong_field_count_is_rejected_not_reinterpreted);
  RUN_TEST(test_v1_exact_field_count_is_accepted);
  RUN_TEST(test_unknown_future_version_falls_back_to_newest_known_layout);
  RUN_TEST(test_wantsCamera_true_when_allCameras);
  RUN_TEST(test_wantsCamera_true_for_listed_camera_case_insensitive);
  RUN_TEST(test_wantsCamera_false_for_unlisted_camera);
  return UNITY_END();
}
