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
  u.canReset = true;
  u.maxCommandsPerMinute = 10;
  u.language = TelegramLang::Portuguese;
  u.canBackup = true;
  u.canRestore = true;
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
  TEST_ASSERT_EQUAL(original.canReset, restored.canReset);
  TEST_ASSERT_EQUAL_UINT32(original.maxCommandsPerMinute, restored.maxCommandsPerMinute);
  TEST_ASSERT_TRUE(original.language == restored.language);
  TEST_ASSERT_EQUAL(original.canBackup, restored.canBackup);
  TEST_ASSERT_EQUAL(original.canRestore, restored.canRestore);
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

// ---- Version 1 (superseded by V2's canReset, still read for migration) ----

// Historical schema-1 data (7 fields, no canReset field at all) must still
// parse correctly when explicitly tagged as version 1 - canReset defaults
// to false (TelegramUser's own default) since it didn't exist yet.
void test_v1_record_parses_with_canReset_defaulted_false(void) {
  String v1 = joinFields({"User", "42", "1", "", "1", "1", "0"}); // 7 fields
  TelegramUser restored = deserializeUser(v1, 1);
  TEST_ASSERT_EQUAL_STRING("User", restored.name.c_str());
  TEST_ASSERT_TRUE(restored.canCommand);
  TEST_ASSERT_FALSE(restored.canReset);
}

// Every field different from its default, so a wrong column shows up.
static const char* const kAllFields[] = {"n", "42", "1", "A\x1D" "B", "1", "1", "1", "1", "9", "1", "1", "1"};
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
  struct { uint16_t version; size_t count; } kLayouts[] = {{1, 7}, {2, 8}, {3, 9}, {4, 10}, {5, 11}, {6, 12}};
  std::vector<String> defaults = columns(serializeUser(TelegramUser()));
  TEST_ASSERT_EQUAL((int)kAllFieldCount, (int)defaults.size());

  for (auto& layout : kLayouts) {
    char label[32];
    snprintf(label, sizeof(label), "version %u", (unsigned)layout.version);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", deserializeUser(firstFields(layout.count - 1), layout.version).name.c_str(), label);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", deserializeUser(firstFields(layout.count + 1), layout.version).name.c_str(), label);

    std::vector<String> got = columns(serializeUser(deserializeUser(firstFields(layout.count), layout.version)));
    for (size_t i = 0; i < kAllFieldCount; i++) {
      const char* expected = i < layout.count ? kAllFields[i] : defaults[i].c_str();
      TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, got[i].c_str(), label);
    }
  }
}

void test_unknown_future_version_falls_back_to_newest_known_layout(void) {
  String record = joinFields({"User", "42", "1", "", "1", "1", "1", "1", "20", "1", "1", "1"}); // 12 fields
  TelegramUser restored = deserializeUser(record, (uint16_t)(TELEGRAM_USER_SCHEMA_VERSION + 1));
  TEST_ASSERT_EQUAL_STRING("User", restored.name.c_str());
  TEST_ASSERT_TRUE(restored.canSnap);
  TEST_ASSERT_TRUE(restored.canReset);
  TEST_ASSERT_EQUAL_UINT32(20, restored.maxCommandsPerMinute);
  TEST_ASSERT_TRUE(TelegramLang::Portuguese == restored.language);
  TEST_ASSERT_TRUE(restored.canBackup);
  TEST_ASSERT_TRUE(restored.canRestore);
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

// The stored layout itself: every position, in order.
void test_stored_layout_is_pinned(void) {
  TelegramUser u;
  u.name = "n"; u.chatId = "42"; u.allCameras = true; u.cameraNames = {"A", "B"};
  u.systemMessages = false; u.canCommand = true; u.canSnap = false; u.canReset = true;
  u.maxCommandsPerMinute = 7; u.language = TelegramLang::Portuguese; u.canBackup = false; u.canRestore = true;

  const char* expected = "n\x1F" "42\x1F" "1\x1F" "A\x1D" "B\x1F" "0\x1F" "1\x1F" "0\x1F" "1\x1F" "7\x1F" "1\x1F" "0\x1F" "1";
  TEST_ASSERT_EQUAL_STRING(expected, serializeUser(u).c_str());
  TEST_ASSERT_EQUAL_STRING(expected, serializeUser(deserializeUser(expected, TELEGRAM_USER_SCHEMA_VERSION)).c_str());
}

void test_current_layout_matches_the_schema_version(void) {
  // A row added without bumping the version would write records this build
  // then refuses to read.
  TelegramUser u;
  u.name = "n";
  TEST_ASSERT_EQUAL_STRING("n", deserializeUser(serializeUser(u), TELEGRAM_USER_SCHEMA_VERSION).name.c_str());
}

void test_empty_rate_limit_keeps_the_default(void) {
  const char* record = "n\x1F" "42\x1F" "1\x1F\x1F" "0\x1F" "0\x1F" "0\x1F" "0\x1F\x1F" "0\x1F" "0\x1F" "0";
  TelegramUser u = deserializeUser(record, TELEGRAM_USER_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("n", u.name.c_str());
  TEST_ASSERT_EQUAL_UINT16(TelegramUser().maxCommandsPerMinute, u.maxCommandsPerMinute);
}

// A stray list separator would split one camera name into two on reload.
void test_separators_inside_values_are_stripped(void) {
  TelegramUser u;
  u.name = "a\x1D" "b";
  u.chatId = "1\x1D" "2";
  u.cameraNames = {"x\x1D" "y"};
  TelegramUser back = deserializeUser(serializeUser(u), TELEGRAM_USER_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("ab", back.name.c_str());
  TEST_ASSERT_EQUAL_STRING("12", back.chatId.c_str());
  TEST_ASSERT_EQUAL(1, (int)back.cameraNames.size());
  TEST_ASSERT_EQUAL_STRING("xy", back.cameraNames[0].c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_every_version_reads_exactly_its_own_fields);
  RUN_TEST(test_separators_inside_values_are_stripped);
  RUN_TEST(test_stored_layout_is_pinned);
  RUN_TEST(test_current_layout_matches_the_schema_version);
  RUN_TEST(test_empty_rate_limit_keeps_the_default);
  RUN_TEST(test_round_trip_preserves_every_field);
  RUN_TEST(test_round_trip_allCameras_true_with_empty_camera_list);
  RUN_TEST(test_malformed_record_returns_empty_name_v0);
  RUN_TEST(test_malformed_record_returns_empty_name_current_version);
  RUN_TEST(test_v0_6_field_record_defaults_canSnap_false);
  RUN_TEST(test_v1_record_parses_with_canReset_defaulted_false);
  RUN_TEST(test_unknown_future_version_falls_back_to_newest_known_layout);
  RUN_TEST(test_wantsCamera_true_when_allCameras);
  RUN_TEST(test_wantsCamera_true_for_listed_camera_case_insensitive);
  RUN_TEST(test_wantsCamera_false_for_unlisted_camera);
  return UNITY_END();
}
