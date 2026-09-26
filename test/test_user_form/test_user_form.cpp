#include <unity.h>
#include "fixtures.h"
#include "golden.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

// ---- Rendering: byte-identical to the captured pre-refactor output ----

void test_render_add_blank(void) {
  TEST_ASSERT_EQUAL_STRING(kGoldenAddBlank, renderTelegramUserForm(fixtureAddBlank(), fixtureCameras(), false).c_str());
}

void test_render_add_no_cameras(void) {
  TEST_ASSERT_EQUAL_STRING(kGoldenAddNoCameras, renderTelegramUserForm(fixtureAddBlank(), {}, false).c_str());
}

void test_render_edit_hostile(void) {
  TEST_ASSERT_EQUAL_STRING(kGoldenEditHostile,
                           renderTelegramUserForm(fixtureEditHostile(), fixtureCameras(), true).c_str());
}

// ---- Parsing ----

void test_parse_empty_form(void) {
  MapParams p;
  TelegramUser u = parseUserForm(p, fixtureCameras());
  TEST_ASSERT_EQUAL_STRING("", u.name.c_str());
  TEST_ASSERT_FALSE(u.allCameras);
  TEST_ASSERT_FALSE(u.systemMessages);
  TEST_ASSERT_FALSE(u.canCommand);
  TEST_ASSERT_FALSE(u.canRestore);
  TEST_ASSERT_EQUAL_UINT16(0, u.maxCommandsPerMinute);
  TEST_ASSERT_TRUE(u.language == TelegramLang::English);
  TEST_ASSERT_EQUAL(0, (int)u.cameraNames.size());
}

void test_parse_all_fields_and_trims(void) {
  MapParams p;
  p.values = {{"name", " Ana "},        {"chatId", " 123 "},   {"systemMessages", "on"},
              {"canCommand", "on"},     {"canSnap", "on"},     {"canReset", "on"},
              {"canBackup", "on"},      {"canRestore", "on"},  {"maxCommandsPerMinute", "12"},
              {"language", "pt"},       {"cam_D01-Front", "on"}, {"cam_d03-garage", "on"},
              {"cam_NotACamera", "on"}};
  TelegramUser u = parseUserForm(p, fixtureCameras());
  TEST_ASSERT_EQUAL_STRING("Ana", u.name.c_str());
  TEST_ASSERT_EQUAL_STRING("123", u.chatId.c_str());
  TEST_ASSERT_TRUE(u.systemMessages && u.canCommand && u.canSnap && u.canReset && u.canBackup && u.canRestore);
  TEST_ASSERT_EQUAL_UINT16(12, u.maxCommandsPerMinute);
  TEST_ASSERT_TRUE(u.language == TelegramLang::Portuguese);
  TEST_ASSERT_EQUAL(2, (int)u.cameraNames.size());  // unknown camera ignored
  TEST_ASSERT_EQUAL_STRING("D01-Front", u.cameraNames[0].c_str());
  TEST_ASSERT_EQUAL_STRING("d03-garage", u.cameraNames[1].c_str());
}

void test_parse_all_cameras_ignores_individual_picks(void) {
  MapParams p;
  p.values = {{"allCameras", "on"}, {"cam_D01-Front", "on"}};
  TelegramUser u = parseUserForm(p, fixtureCameras());
  TEST_ASSERT_TRUE(u.allCameras);
  TEST_ASSERT_EQUAL(0, (int)u.cameraNames.size());
}

void test_parse_clamps_rate_limit(void) {
  MapParams p;
  p.values = {{"maxCommandsPerMinute", "-4"}};
  TEST_ASSERT_EQUAL_UINT16(0, parseUserForm(p, {}).maxCommandsPerMinute);
  p.values = {{"maxCommandsPerMinute", "99999"}};
  TEST_ASSERT_EQUAL_UINT16(TELEGRAM_MAX_COMMANDS_PER_MINUTE_MAX, parseUserForm(p, {}).maxCommandsPerMinute);
}

void test_parse_unknown_language_is_english(void) {
  MapParams p;
  p.values = {{"language", "fr"}};
  TEST_ASSERT_TRUE(parseUserForm(p, {}).language == TelegramLang::English);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_render_add_blank);
  RUN_TEST(test_render_add_no_cameras);
  RUN_TEST(test_render_edit_hostile);
  RUN_TEST(test_parse_empty_form);
  RUN_TEST(test_parse_all_fields_and_trims);
  RUN_TEST(test_parse_all_cameras_ignores_individual_picks);
  RUN_TEST(test_parse_clamps_rate_limit);
  RUN_TEST(test_parse_unknown_language_is_english);
  return UNITY_END();
}
