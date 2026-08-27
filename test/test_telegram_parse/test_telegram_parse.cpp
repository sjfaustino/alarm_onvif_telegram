#include <unity.h>
#include <Arduino.h>
#include "telegram_parse.h"

void setUp(void) {}
void tearDown(void) {}

// ---- jsonEscape / jsonUnescape ----

void test_jsonEscape_escapes_quote_backslash_and_control_chars(void) {
  TEST_ASSERT_EQUAL_STRING("say \\\"hi\\\"\\n\\ttab\\\\slash",
                            jsonEscape("say \"hi\"\n\ttab\\slash").c_str());
}

void test_jsonEscape_leaves_plain_text_untouched(void) {
  TEST_ASSERT_EQUAL_STRING("D01 alerts: ON", jsonEscape("D01 alerts: ON").c_str());
}

void test_jsonUnescape_is_the_inverse_of_jsonEscape(void) {
  String original = "say \"hi\"\n\ttab\\slash";
  TEST_ASSERT_EQUAL_STRING(original.c_str(), jsonUnescape(jsonEscape(original)).c_str());
}

// A trailing lone backslash (malformed/truncated input) must not read past
// the end of the string - jsonUnescape's own bounds check
// (i + 1 < in.length()) is what this exercises.
void test_jsonUnescape_trailing_lone_backslash_does_not_crash(void) {
  String r = jsonUnescape("abc\\");
  TEST_ASSERT_EQUAL_STRING("abc\\", r.c_str());
}

// ---- matchCamerasByPrefix ----

static CameraConfig makeCam(const char* name, bool enabled = true) {
  CameraConfig c;
  c.name = name;
  c.enabled = enabled;
  return c;
}

void test_matchCamerasByPrefix_single_prefix_match(void) {
  CameraConfig cams[] = {makeCam("D01-FrontDoor"), makeCam("D02-BackGate")};
  std::vector<size_t> matches = matchCamerasByPrefix(cams, 2, "D01");
  TEST_ASSERT_EQUAL_INT(1, (int)matches.size());
  TEST_ASSERT_EQUAL_INT(0, (int)matches[0]);
}

void test_matchCamerasByPrefix_is_case_insensitive(void) {
  CameraConfig cams[] = {makeCam("D01-FrontDoor")};
  std::vector<size_t> matches = matchCamerasByPrefix(cams, 1, "d01-frontdoor");
  TEST_ASSERT_EQUAL_INT(1, (int)matches.size());
}

void test_matchCamerasByPrefix_exact_name_matches_itself(void) {
  CameraConfig cams[] = {makeCam("D01")};
  std::vector<size_t> matches = matchCamerasByPrefix(cams, 1, "D01");
  TEST_ASSERT_EQUAL_INT(1, (int)matches.size());
}

void test_matchCamerasByPrefix_ambiguous_prefix_returns_all_matches(void) {
  CameraConfig cams[] = {makeCam("D01-FrontDoor"), makeCam("D01-Garage")};
  std::vector<size_t> matches = matchCamerasByPrefix(cams, 2, "D01");
  TEST_ASSERT_EQUAL_INT(2, (int)matches.size());
}

void test_matchCamerasByPrefix_no_match_returns_empty(void) {
  CameraConfig cams[] = {makeCam("D01-FrontDoor")};
  std::vector<size_t> matches = matchCamerasByPrefix(cams, 1, "D99");
  TEST_ASSERT_TRUE(matches.empty());
}

// A disabled camera must never be a valid /on, /off, or /snap target - the
// dashboard already won't run its task, so toggling or snapping it would
// be silently pointless at best.
void test_matchCamerasByPrefix_excludes_disabled_cameras(void) {
  CameraConfig cams[] = {makeCam("D01-FrontDoor", /*enabled=*/false)};
  std::vector<size_t> matches = matchCamerasByPrefix(cams, 1, "D01");
  TEST_ASSERT_TRUE(matches.empty());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_jsonEscape_escapes_quote_backslash_and_control_chars);
  RUN_TEST(test_jsonEscape_leaves_plain_text_untouched);
  RUN_TEST(test_jsonUnescape_is_the_inverse_of_jsonEscape);
  RUN_TEST(test_jsonUnescape_trailing_lone_backslash_does_not_crash);
  RUN_TEST(test_matchCamerasByPrefix_single_prefix_match);
  RUN_TEST(test_matchCamerasByPrefix_is_case_insensitive);
  RUN_TEST(test_matchCamerasByPrefix_exact_name_matches_itself);
  RUN_TEST(test_matchCamerasByPrefix_ambiguous_prefix_returns_all_matches);
  RUN_TEST(test_matchCamerasByPrefix_no_match_returns_empty);
  RUN_TEST(test_matchCamerasByPrefix_excludes_disabled_cameras);
  return UNITY_END();
}
