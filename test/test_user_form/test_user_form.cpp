#include <unity.h>
#include "fixtures.h"
#include "html_form_reader.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

// ---- Rendering: what a browser submits, and what the page promises ----

static TelegramUser submit(const TelegramUser& u, const std::vector<CameraConfig>& cams, bool isEdit) {
  return parseUserForm(BrowserSubmission(renderTelegramUserForm(u, cams, isEdit)), cams);
}

void test_round_trip_keeps_identity_rate_and_language(void) {
  TelegramUser u = fixtureEditHostile();
  TelegramUser r = submit(u, fixtureCameras(), true);
  TEST_ASSERT_EQUAL_STRING(u.name.c_str(), r.name.c_str());
  TEST_ASSERT_EQUAL_STRING(u.chatId.c_str(), r.chatId.c_str());
  TEST_ASSERT_EQUAL_UINT16(12, r.maxCommandsPerMinute);
  TEST_ASSERT_TRUE(r.language == TelegramLang::Portuguese);
  TEST_ASSERT_TRUE(submit(fixtureAddBlank(), fixtureCameras(), false).language == TelegramLang::English);
}

// A swapped checkbox could still round-trip, and these are permissions, so tie
// each field's name to its setting in both directions.
void test_each_permission_name_controls_its_own_setting(void) {
  const struct { const char* name; bool TelegramUser::*flag; } boxes[] = {
      {"allCameras", &TelegramUser::allCameras},
      {"systemMessages", &TelegramUser::systemMessages},
      {"canCommand", &TelegramUser::canCommand},
      {"canSnap", &TelegramUser::canSnap},
      {"canReset", &TelegramUser::canReset},
      {"canBackup", &TelegramUser::canBackup},
      {"canRestore", &TelegramUser::canRestore}};
  for (auto& on : boxes) {
    TelegramUser u;
    for (auto& b : boxes) u.*b.flag = false;
    u.*on.flag = true;
    BrowserSubmission sent(renderTelegramUserForm(u, fixtureCameras(), false));
    MapParams only;
    only.values = {{on.name, "on"}};
    TelegramUser parsed = parseUserForm(only, fixtureCameras());
    for (auto& b : boxes) {
      bool expected = b.flag == on.flag;
      TEST_ASSERT_EQUAL_MESSAGE(expected, sent.has(b.name), b.name);
      TEST_ASSERT_EQUAL_MESSAGE(expected, parsed.*b.flag, b.name);
    }
  }
}

void test_round_trip_keeps_camera_picks_including_hostile_names(void) {
  std::vector<CameraConfig> cams = fixtureCameras();
  TelegramUser u = fixtureEditHostile();
  u.cameraNames = {"d01-FRONT", cams[1].name};  // first differs in case only
  TelegramUser r = submit(u, cams, true);
  TEST_ASSERT_EQUAL(2, (int)r.cameraNames.size());
  TEST_ASSERT_EQUAL_STRING("D01-Front", r.cameraNames[0].c_str());
  TEST_ASSERT_EQUAL_STRING(cams[1].name.c_str(), r.cameraNames[1].c_str());
}

void test_no_field_name_is_used_twice(void) {
  BrowserSubmission b(renderTelegramUserForm(fixtureEditHostile(), fixtureCameras(), true));
  TEST_ASSERT_EQUAL(0, (int)b.duplicates().size());
}

void test_hostile_text_is_escaped(void) {
  String html = renderTelegramUserForm(fixtureEditHostile(), fixtureCameras(), true);
  TEST_ASSERT_EQUAL(-1, html.indexOf("Ana <"));
  TEST_ASSERT_EQUAL(-1, html.indexOf("D02 <"));
  TEST_ASSERT_TRUE(html.indexOf("D02 &lt;&quot;Back&quot;&gt;") >= 0);
}

void test_no_cameras_shows_a_hint(void) {
  TEST_ASSERT_TRUE(renderTelegramUserForm(fixtureAddBlank(), {}, false).indexOf("No cameras defined yet") >= 0);
  TEST_ASSERT_EQUAL(-1, renderTelegramUserForm(fixtureAddBlank(), fixtureCameras(), false).indexOf("No cameras defined yet"));
}

void test_edit_form_has_original_name_and_cancel_add_form_does_not(void) {
  String edit = renderTelegramUserForm(fixtureEditHostile(), fixtureCameras(), true);
  String add = renderTelegramUserForm(fixtureAddBlank(), fixtureCameras(), false);
  TEST_ASSERT_EQUAL_STRING(fixtureEditHostile().name.c_str(), BrowserSubmission(edit).get("originalName", "").c_str());
  TEST_ASSERT_FALSE(BrowserSubmission(add).has("originalName"));
  TEST_ASSERT_TRUE(edit.indexOf(">Cancel<") >= 0);
  TEST_ASSERT_EQUAL(-1, add.indexOf(">Cancel<"));
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
  RUN_TEST(test_round_trip_keeps_identity_rate_and_language);
  RUN_TEST(test_each_permission_name_controls_its_own_setting);
  RUN_TEST(test_round_trip_keeps_camera_picks_including_hostile_names);
  RUN_TEST(test_no_field_name_is_used_twice);
  RUN_TEST(test_hostile_text_is_escaped);
  RUN_TEST(test_no_cameras_shows_a_hint);
  RUN_TEST(test_edit_form_has_original_name_and_cancel_add_form_does_not);
  RUN_TEST(test_parse_empty_form);
  RUN_TEST(test_parse_all_fields_and_trims);
  RUN_TEST(test_parse_all_cameras_ignores_individual_picks);
  RUN_TEST(test_parse_clamps_rate_limit);
  RUN_TEST(test_parse_unknown_language_is_english);
  return UNITY_END();
}
