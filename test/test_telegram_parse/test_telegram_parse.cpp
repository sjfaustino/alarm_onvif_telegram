#include <unity.h>
#include <Arduino.h>
#include "telegram_parse.h"

void setUp(void) {}
void tearDown(void) {}

// ---- parseTelegramUpdates ----
// Fixtures are trimmed but real shapes of Telegram's getUpdates response -
// see https://core.telegram.org/bots/api#getupdates / #update / #message.

void test_parseTelegramUpdates_single_update(void) {
  String body = R"({"ok":true,"result":[
    {"update_id":100,"message":{"message_id":1,"chat":{"id":123456789,"type":"private"},
     "date":1,"text":"/status"}}
  ]})";
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body);
  TEST_ASSERT_EQUAL_INT(1, (int)updates.size());
  TEST_ASSERT_EQUAL_INT32(100, updates[0].updateId);
  TEST_ASSERT_TRUE(updates[0].hasChatId);
  TEST_ASSERT_EQUAL_INT64(123456789, updates[0].chatId);
  TEST_ASSERT_EQUAL_STRING("/status", updates[0].text.c_str());
}

// Real Telegram chat IDs for ordinary accounts (not just groups/channels)
// routinely exceed 32-bit range (~2.1 billion) - chatId must survive a
// value that would silently overflow/truncate a `long` on this platform.
// This is the exact bug hit in the field: a chat ID this size came back
// as 0, matching no configured user.
void test_parseTelegramUpdates_chat_id_beyond_32_bits(void) {
  String body = R"({"ok":true,"result":[
    {"update_id":1,"message":{"chat":{"id":8897455184},"text":"/status"}}
  ]})";
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body);
  TEST_ASSERT_EQUAL_INT64(8897455184LL, updates[0].chatId);
}

void test_parseTelegramUpdates_multiple_updates_in_order(void) {
  String body = R"({"ok":true,"result":[
    {"update_id":1,"message":{"chat":{"id":10},"text":"/on D01"}},
    {"update_id":2,"message":{"chat":{"id":10},"text":"/off D01"}},
    {"update_id":3,"message":{"chat":{"id":20},"text":"/snap D02"}}
  ]})";
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body);
  TEST_ASSERT_EQUAL_INT(3, (int)updates.size());
  TEST_ASSERT_EQUAL_STRING("/on D01", updates[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("/off D01", updates[1].text.c_str());
  TEST_ASSERT_EQUAL_INT64(20, updates[2].chatId);
}

// A JSON-escaped quote/backslash/newline in the text field must come back
// as the real character, not the escaped two-character sequence - this is
// what the old hand-rolled jsonUnescape() used to be responsible for;
// ArduinoJson does it as a normal part of deserializeJson().
void test_parseTelegramUpdates_unescapes_text_field(void) {
  String body = R"({"ok":true,"result":[
    {"update_id":1,"message":{"chat":{"id":1},"text":"line one\nline \"two\"\\end"}}
  ]})";
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body);
  TEST_ASSERT_EQUAL_STRING("line one\nline \"two\"\\end", updates[0].text.c_str());
}

// An update with no "message" at all (edited_message, channel_post, ...)
// still has its update_id returned so the caller can advance its offset
// past it - only hasChatId/text signal "nothing to act on" here.
void test_parseTelegramUpdates_update_without_message_still_returns_updateId(void) {
  String body = R"({"ok":true,"result":[
    {"update_id":5,"edited_message":{"chat":{"id":1},"text":"edited"}}
  ]})";
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body);
  TEST_ASSERT_EQUAL_INT(1, (int)updates.size());
  TEST_ASSERT_EQUAL_INT32(5, updates[0].updateId);
  TEST_ASSERT_FALSE(updates[0].hasChatId);
  TEST_ASSERT_EQUAL_STRING("", updates[0].text.c_str());
}

// A message with no text (a sticker, a photo with no caption) has a chat
// id but no text - the caller treats an empty text as "nothing to act on"
// too, but the chat id (and update id) are still real and usable.
void test_parseTelegramUpdates_message_without_text(void) {
  String body = R"({"ok":true,"result":[
    {"update_id":7,"message":{"chat":{"id":42},"sticker":{"file_id":"abc"}}}
  ]})";
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body);
  TEST_ASSERT_EQUAL_INT(1, (int)updates.size());
  TEST_ASSERT_TRUE(updates[0].hasChatId);
  TEST_ASSERT_EQUAL_STRING("", updates[0].text.c_str());
}

void test_parseTelegramUpdates_empty_result_array(void) {
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(R"({"ok":true,"result":[]})");
  TEST_ASSERT_TRUE(updates.empty());
}

// Malformed JSON (truncated, not JSON at all, ...) must not crash - it's
// reported via the optional `error` out-param and an empty result, not a
// thrown exception (this project builds without C++ exceptions) or a
// garbage partial parse.
void test_parseTelegramUpdates_malformed_json_reports_error(void) {
  String error;
  std::vector<TelegramUpdate> updates = parseTelegramUpdates("{not valid json", &error);
  TEST_ASSERT_TRUE(updates.empty());
  TEST_ASSERT_TRUE(error.length() > 0);
}

// Telegram reports API-level failures (e.g. an invalid bot token) as
// {"ok":false,...} with HTTP 200 - no exception, no malformed JSON, just a
// false "ok". Must come back as an error, not silently as "zero updates".
void test_parseTelegramUpdates_api_error_reports_description(void) {
  String error;
  std::vector<TelegramUpdate> updates =
      parseTelegramUpdates(R"({"ok":false,"error_code":401,"description":"Unauthorized"})", &error);
  TEST_ASSERT_TRUE(updates.empty());
  TEST_ASSERT_TRUE(error.indexOf("Unauthorized") >= 0);
}

// error is optional - passing nullptr (the default) must not crash even
// when there's something to report.
void test_parseTelegramUpdates_null_error_param_is_optional(void) {
  std::vector<TelegramUpdate> updates = parseTelegramUpdates("{not valid json");
  TEST_ASSERT_TRUE(updates.empty());
}

// ---- chatIdMatches ----

void test_chatIdMatches_equal_ids(void) {
  TEST_ASSERT_TRUE(chatIdMatches("123456789", 123456789LL));
}

void test_chatIdMatches_different_ids(void) {
  TEST_ASSERT_FALSE(chatIdMatches("123456789", 987654321LL));
}

// The actual bug: a stored chat ID beyond 32-bit range must still match
// correctly - String::toInt() (also only 32-bit here) was the other half
// of this bug alongside TelegramUpdate::chatId itself being a `long`.
void test_chatIdMatches_beyond_32_bits(void) {
  TEST_ASSERT_TRUE(chatIdMatches("8897455184", 8897455184LL));
  TEST_ASSERT_FALSE(chatIdMatches("8897455184", 8897455185LL));
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
  RUN_TEST(test_parseTelegramUpdates_single_update);
  RUN_TEST(test_parseTelegramUpdates_chat_id_beyond_32_bits);
  RUN_TEST(test_parseTelegramUpdates_multiple_updates_in_order);
  RUN_TEST(test_parseTelegramUpdates_unescapes_text_field);
  RUN_TEST(test_parseTelegramUpdates_update_without_message_still_returns_updateId);
  RUN_TEST(test_parseTelegramUpdates_message_without_text);
  RUN_TEST(test_parseTelegramUpdates_empty_result_array);
  RUN_TEST(test_parseTelegramUpdates_malformed_json_reports_error);
  RUN_TEST(test_parseTelegramUpdates_api_error_reports_description);
  RUN_TEST(test_parseTelegramUpdates_null_error_param_is_optional);
  RUN_TEST(test_chatIdMatches_equal_ids);
  RUN_TEST(test_chatIdMatches_different_ids);
  RUN_TEST(test_chatIdMatches_beyond_32_bits);
  RUN_TEST(test_matchCamerasByPrefix_single_prefix_match);
  RUN_TEST(test_matchCamerasByPrefix_is_case_insensitive);
  RUN_TEST(test_matchCamerasByPrefix_exact_name_matches_itself);
  RUN_TEST(test_matchCamerasByPrefix_ambiguous_prefix_returns_all_matches);
  RUN_TEST(test_matchCamerasByPrefix_no_match_returns_empty);
  RUN_TEST(test_matchCamerasByPrefix_excludes_disabled_cameras);
  return UNITY_END();
}
