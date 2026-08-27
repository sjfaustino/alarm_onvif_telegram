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
void test_round_trip_preserves_every_field(void) {
  TelegramUser original = sampleUser();
  TelegramUser restored = deserializeUser(serializeUser(original));

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
  TelegramUser restored = deserializeUser(serializeUser(u));
  TEST_ASSERT_TRUE(restored.allCameras);
  TEST_ASSERT_EQUAL_INT(0, (int)restored.cameraNames.size());
}

// deserializeUser's caller (loadTelegramUsers()) treats an empty name as
// "malformed, skip this entry".
void test_malformed_record_returns_empty_name(void) {
  TelegramUser u = deserializeUser("only|two");
  TEST_ASSERT_EQUAL_STRING("", u.name.c_str());
}

// Records saved before canSnap existed have only the original 6 fields -
// loading one must default canSnap to false (TelegramUser's own default),
// not crash or misparse the rest.
void test_legacy_6_field_record_defaults_canSnap_false(void) {
  String legacy = joinFields({"OldUser", "42", "1", "", "1", "1"});
  TelegramUser restored = deserializeUser(legacy);

  TEST_ASSERT_EQUAL_STRING("OldUser", restored.name.c_str());
  TEST_ASSERT_TRUE(restored.canCommand);
  TEST_ASSERT_FALSE(restored.canSnap);
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
  RUN_TEST(test_malformed_record_returns_empty_name);
  RUN_TEST(test_legacy_6_field_record_defaults_canSnap_false);
  RUN_TEST(test_wantsCamera_true_when_allCameras);
  RUN_TEST(test_wantsCamera_true_for_listed_camera_case_insensitive);
  RUN_TEST(test_wantsCamera_false_for_unlisted_camera);
  return UNITY_END();
}
