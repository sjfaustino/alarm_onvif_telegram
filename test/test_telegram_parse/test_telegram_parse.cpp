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

// An inline-keyboard button tap - callback_query, not message. chatId
// comes from callback_query.message.chat.id (the chat the picker message
// was sent to), same field names/roles as a normal message update.
void test_parseTelegramUpdates_callback_query(void) {
  String body = R"({"ok":true,"result":[
    {"update_id":9,"callback_query":{"id":"cbq123","data":"off|D01-FrontDoor",
     "message":{"chat":{"id":555}}}}
  ]})";
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body);
  TEST_ASSERT_EQUAL_INT(1, (int)updates.size());
  TEST_ASSERT_TRUE(updates[0].hasCallbackQuery);
  TEST_ASSERT_EQUAL_STRING("cbq123", updates[0].callbackQueryId.c_str());
  TEST_ASSERT_EQUAL_STRING("off|D01-FrontDoor", updates[0].callbackData.c_str());
  TEST_ASSERT_TRUE(updates[0].hasChatId);
  TEST_ASSERT_EQUAL_INT64(555, updates[0].chatId);
  TEST_ASSERT_EQUAL_STRING("", updates[0].text.c_str());
}

// Telegram can omit "message" on a callback_query for a stale/deleted
// message - hasChatId must stay false (not crash), same null-safety
// idiom as a plain message's missing chat.
void test_parseTelegramUpdates_callback_query_without_message(void) {
  String body = R"({"ok":true,"result":[
    {"update_id":10,"callback_query":{"id":"cbq456","data":"snap|all"}}
  ]})";
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body);
  TEST_ASSERT_TRUE(updates[0].hasCallbackQuery);
  TEST_ASSERT_FALSE(updates[0].hasChatId);
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

// ---- requiredPermissionForCommand ----
// The single source of truth for "which permission does this command
// need" - handleTelegramCommand's authorization check is built from this,
// so this table is the one place a new command's permission requirement
// has to be declared, instead of a separate easy-to-forget check per
// command (the exact class of bug that caused the /reset reboot loop).

void test_requiredPermissionForCommand_status_uptime_on_off_need_command(void) {
  TEST_ASSERT_TRUE(TelegramCommandPermission::Command == requiredPermissionForCommand(TelegramCommand::Status));
  TEST_ASSERT_TRUE(TelegramCommandPermission::Command == requiredPermissionForCommand(TelegramCommand::Uptime));
  TEST_ASSERT_TRUE(TelegramCommandPermission::Command == requiredPermissionForCommand(TelegramCommand::On));
  TEST_ASSERT_TRUE(TelegramCommandPermission::Command == requiredPermissionForCommand(TelegramCommand::Off));
}

void test_requiredPermissionForCommand_snap_needs_snap(void) {
  TEST_ASSERT_TRUE(TelegramCommandPermission::Snap == requiredPermissionForCommand(TelegramCommand::Snap));
}

void test_requiredPermissionForCommand_reset_needs_reset(void) {
  TEST_ASSERT_TRUE(TelegramCommandPermission::Reset == requiredPermissionForCommand(TelegramCommand::Reset));
}

void test_requiredPermissionForCommand_unknown_needs_unknown(void) {
  TEST_ASSERT_TRUE(TelegramCommandPermission::Unknown == requiredPermissionForCommand(TelegramCommand::Unknown));
}

// ---- parseTelegramCommand ----
// The single place message text is matched against command syntax -
// handleTelegramCommand used to parse this same text twice (once for
// authorization, again to dispatch); this is now the only place it happens.

void test_parseTelegramCommand_status(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/status");
  TEST_ASSERT_TRUE(TelegramCommand::Status == p.command);
  TEST_ASSERT_TRUE(TelegramCommandPermission::Command == p.requiredPermission);
  TEST_ASSERT_EQUAL_STRING("", p.cameraName.c_str());
}

void test_parseTelegramCommand_uptime(void) {
  TEST_ASSERT_TRUE(TelegramCommand::Uptime == parseTelegramCommand("/uptime").command);
}

void test_parseTelegramCommand_reset(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/reset");
  TEST_ASSERT_TRUE(TelegramCommand::Reset == p.command);
  TEST_ASSERT_TRUE(TelegramCommandPermission::Reset == p.requiredPermission);
}

void test_parseTelegramCommand_on_extracts_and_trims_camera_name(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/on  D01-FrontDoor  ");
  TEST_ASSERT_TRUE(TelegramCommand::On == p.command);
  TEST_ASSERT_TRUE(TelegramCommandPermission::Command == p.requiredPermission);
  TEST_ASSERT_EQUAL_STRING("D01-FrontDoor", p.cameraName.c_str());
}

void test_parseTelegramCommand_off(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/off D02");
  TEST_ASSERT_TRUE(TelegramCommand::Off == p.command);
  TEST_ASSERT_EQUAL_STRING("D02", p.cameraName.c_str());
}

void test_parseTelegramCommand_snap(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/snap D03");
  TEST_ASSERT_TRUE(TelegramCommand::Snap == p.command);
  TEST_ASSERT_TRUE(TelegramCommandPermission::Snap == p.requiredPermission);
  TEST_ASSERT_EQUAL_STRING("D03", p.cameraName.c_str());
}

void test_parseTelegramCommand_help(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/help");
  TEST_ASSERT_TRUE(TelegramCommand::Help == p.command);
  TEST_ASSERT_TRUE(TelegramCommandPermission::Unknown == p.requiredPermission);
}

void test_parseTelegramCommand_health(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/health");
  TEST_ASSERT_TRUE(TelegramCommand::Health == p.command);
  TEST_ASSERT_TRUE(TelegramCommandPermission::Command == p.requiredPermission);
}

void test_parseTelegramCommand_log_bare(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/log");
  TEST_ASSERT_TRUE(TelegramCommand::Log == p.command);
  TEST_ASSERT_TRUE(TelegramCommandPermission::Command == p.requiredPermission);
  TEST_ASSERT_EQUAL_STRING("", p.logCountText.c_str());
}

void test_parseTelegramCommand_log_with_count(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/log 20");
  TEST_ASSERT_TRUE(TelegramCommand::Log == p.command);
  TEST_ASSERT_EQUAL_STRING("20", p.logCountText.c_str());
}

void test_parseTelegramCommand_is_case_insensitive(void) {
  TEST_ASSERT_TRUE(TelegramCommand::Reset == parseTelegramCommand("/RESET").command);
  TEST_ASSERT_TRUE(TelegramCommand::Snap == parseTelegramCommand("/SNAP D01").command);
}

// Bare "/on"/"/off"/"/snap" (no target at all) parse as their own command
// with an empty cameraName, NOT Unknown - handleTelegramCommand (telegram.cpp)
// recognizes the empty name and sends an inline-keyboard camera picker
// instead of falling through to name/prefix matching.
void test_parseTelegramCommand_on_without_target_is_picker(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/on");
  TEST_ASSERT_TRUE(TelegramCommand::On == p.command);
  TEST_ASSERT_TRUE(TelegramCommandPermission::Command == p.requiredPermission);
  TEST_ASSERT_EQUAL_STRING("", p.cameraName.c_str());
}

void test_parseTelegramCommand_off_without_target_is_picker(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/off");
  TEST_ASSERT_TRUE(TelegramCommand::Off == p.command);
  TEST_ASSERT_EQUAL_STRING("", p.cameraName.c_str());
}

void test_parseTelegramCommand_snap_without_target_is_picker(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/snap");
  TEST_ASSERT_TRUE(TelegramCommand::Snap == p.command);
  TEST_ASSERT_EQUAL_STRING("", p.cameraName.c_str());
}

// "/on " (trailing space, still nothing after it) reaches the same empty
// cameraName via splitNameAndDuration instead of the bare-word branch
// above - a second, previously-untested path into the same picker case.
void test_parseTelegramCommand_on_with_trailing_space_only_is_picker(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/on ");
  TEST_ASSERT_TRUE(TelegramCommand::On == p.command);
  TEST_ASSERT_EQUAL_STRING("", p.cameraName.c_str());
}

void test_parseTelegramCommand_unrecognized_text_is_unknown(void) {
  ParsedTelegramCommand p = parseTelegramCommand("hello");
  TEST_ASSERT_TRUE(TelegramCommand::Unknown == p.command);
  TEST_ASSERT_EQUAL_STRING("", p.cameraName.c_str());
}

// ---- parseTelegramCommand: /on and /off timer syntax ----

void test_parseTelegramCommand_off_with_minutes_duration(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/off D01 30");
  TEST_ASSERT_TRUE(TelegramCommand::Off == p.command);
  TEST_ASSERT_EQUAL_STRING("D01", p.cameraName.c_str());
  TEST_ASSERT_EQUAL_STRING("30", p.durationText.c_str());
}

void test_parseTelegramCommand_on_with_clock_time_duration(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/on D01 23:00");
  TEST_ASSERT_TRUE(TelegramCommand::On == p.command);
  TEST_ASSERT_EQUAL_STRING("D01", p.cameraName.c_str());
  TEST_ASSERT_EQUAL_STRING("23:00", p.durationText.c_str());
}

void test_parseTelegramCommand_off_without_duration_leaves_it_empty(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/off D01");
  TEST_ASSERT_EQUAL_STRING("D01", p.cameraName.c_str());
  TEST_ASSERT_EQUAL_STRING("", p.durationText.c_str());
}

void test_parseTelegramCommand_off_tolerates_extra_whitespace_around_duration(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/off   D01   30  ");
  TEST_ASSERT_EQUAL_STRING("D01", p.cameraName.c_str());
  TEST_ASSERT_EQUAL_STRING("30", p.durationText.c_str());
}

// /snap never accepts a duration - a second token is just part of
// cameraName there (unchanged, pre-existing behavior).
void test_parseTelegramCommand_snap_does_not_split_a_duration(void) {
  ParsedTelegramCommand p = parseTelegramCommand("/snap D03");
  TEST_ASSERT_EQUAL_STRING("", p.durationText.c_str());
}

// ---- parseDurationToken ----

static struct tm makeLocalTime(int year, int mon, int day, int hour, int min, int sec) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = mon - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = min;
  t.tm_sec = sec;
  return t;
}

void test_parseDurationToken_plain_minutes(void) {
  struct tm now = makeLocalTime(2026, 1, 1, 10, 0, 0);
  ParsedDuration d = parseDurationToken("30", now);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL_UINT32(1800UL, d.secondsFromNow);
}

// Minutes form doesn't need a synced clock at all - it never looks at
// nowLocal - so an all-zero/unsynced struct tm must not reject it.
void test_parseDurationToken_plain_minutes_works_without_synced_clock(void) {
  struct tm now = {}; // tm_year 0 -> year 1900, well before the sync threshold
  ParsedDuration d = parseDurationToken("5", now);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL_UINT32(300UL, d.secondsFromNow);
}

void test_parseDurationToken_rejects_zero_minutes(void) {
  struct tm now = makeLocalTime(2026, 1, 1, 10, 0, 0);
  TEST_ASSERT_FALSE(parseDurationToken("0", now).ok);
}

void test_parseDurationToken_rejects_non_numeric_minutes(void) {
  struct tm now = makeLocalTime(2026, 1, 1, 10, 0, 0);
  TEST_ASSERT_FALSE(parseDurationToken("30m", now).ok);
  TEST_ASSERT_FALSE(parseDurationToken("abc", now).ok);
  TEST_ASSERT_FALSE(parseDurationToken("-5", now).ok);
}

void test_parseDurationToken_rejects_empty_token(void) {
  struct tm now = makeLocalTime(2026, 1, 1, 10, 0, 0);
  TEST_ASSERT_FALSE(parseDurationToken("", now).ok);
}

// HH:MM later today - straightforward same-day delta.
void test_parseDurationToken_clock_time_later_today(void) {
  struct tm now = makeLocalTime(2026, 1, 1, 10, 0, 0);
  ParsedDuration d = parseDurationToken("23:00", now);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL_UINT32(13UL * 3600UL, d.secondsFromNow); // 10:00 -> 23:00
}

// HH:MM already passed today - the "smaller than current time -> next
// day" rule from the request this feature was built for.
void test_parseDurationToken_clock_time_already_passed_rolls_to_tomorrow(void) {
  struct tm now = makeLocalTime(2026, 1, 1, 23, 30, 0);
  ParsedDuration d = parseDurationToken("10:00", now);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL_UINT32(10UL * 3600UL + 30UL * 60UL, d.secondsFromNow); // 23:30 -> 10:00 next day
}

// Exactly equal to the current time - also rolls to tomorrow rather than
// scheduling an immediate (zero-delay) revert.
void test_parseDurationToken_clock_time_equal_to_now_rolls_to_tomorrow(void) {
  struct tm now = makeLocalTime(2026, 1, 1, 10, 0, 0);
  ParsedDuration d = parseDurationToken("10:00", now);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL_UINT32(24UL * 3600UL, d.secondsFromNow);
}

void test_parseDurationToken_rejects_out_of_range_clock_time(void) {
  struct tm now = makeLocalTime(2026, 1, 1, 10, 0, 0);
  TEST_ASSERT_FALSE(parseDurationToken("24:00", now).ok);
  TEST_ASSERT_FALSE(parseDurationToken("12:60", now).ok);
}

void test_parseDurationToken_rejects_malformed_clock_time(void) {
  struct tm now = makeLocalTime(2026, 1, 1, 10, 0, 0);
  TEST_ASSERT_FALSE(parseDurationToken("1:30", now).ok);  // hour not 2 digits
  TEST_ASSERT_FALSE(parseDurationToken("12:3", now).ok);  // minute not 2 digits
  TEST_ASSERT_FALSE(parseDurationToken("ab:cd", now).ok);
}

// HH:MM specifically needs a synced clock (unlike plain minutes) -
// resolving "at 23:00" against an unsynced (epoch-default) time-of-day
// would silently schedule against the wrong wall-clock time.
void test_parseDurationToken_clock_time_rejected_when_unsynced(void) {
  struct tm now = {}; // tm_year 0 -> 1900, below the sync threshold
  TEST_ASSERT_FALSE(parseDurationToken("23:00", now).ok);
}

// ---- commandDisplayName ----

void test_commandDisplayName_every_command(void) {
  TEST_ASSERT_EQUAL_STRING("/status", commandDisplayName(TelegramCommand::Status).c_str());
  TEST_ASSERT_EQUAL_STRING("/uptime", commandDisplayName(TelegramCommand::Uptime).c_str());
  TEST_ASSERT_EQUAL_STRING("/reset", commandDisplayName(TelegramCommand::Reset).c_str());
  TEST_ASSERT_EQUAL_STRING("/on", commandDisplayName(TelegramCommand::On).c_str());
  TEST_ASSERT_EQUAL_STRING("/off", commandDisplayName(TelegramCommand::Off).c_str());
  TEST_ASSERT_EQUAL_STRING("/snap", commandDisplayName(TelegramCommand::Snap).c_str());
  TEST_ASSERT_EQUAL_STRING("/help", commandDisplayName(TelegramCommand::Help).c_str());
  TEST_ASSERT_EQUAL_STRING("/health", commandDisplayName(TelegramCommand::Health).c_str());
  TEST_ASSERT_EQUAL_STRING("/log", commandDisplayName(TelegramCommand::Log).c_str());
  TEST_ASSERT_EQUAL_STRING("", commandDisplayName(TelegramCommand::Unknown).c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_parseTelegramUpdates_single_update);
  RUN_TEST(test_parseTelegramUpdates_chat_id_beyond_32_bits);
  RUN_TEST(test_parseTelegramUpdates_multiple_updates_in_order);
  RUN_TEST(test_parseTelegramUpdates_unescapes_text_field);
  RUN_TEST(test_parseTelegramUpdates_update_without_message_still_returns_updateId);
  RUN_TEST(test_parseTelegramUpdates_message_without_text);
  RUN_TEST(test_parseTelegramUpdates_callback_query);
  RUN_TEST(test_parseTelegramUpdates_callback_query_without_message);
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
  RUN_TEST(test_requiredPermissionForCommand_status_uptime_on_off_need_command);
  RUN_TEST(test_requiredPermissionForCommand_snap_needs_snap);
  RUN_TEST(test_requiredPermissionForCommand_reset_needs_reset);
  RUN_TEST(test_requiredPermissionForCommand_unknown_needs_unknown);
  RUN_TEST(test_parseTelegramCommand_status);
  RUN_TEST(test_parseTelegramCommand_uptime);
  RUN_TEST(test_parseTelegramCommand_reset);
  RUN_TEST(test_parseTelegramCommand_on_extracts_and_trims_camera_name);
  RUN_TEST(test_parseTelegramCommand_off);
  RUN_TEST(test_parseTelegramCommand_snap);
  RUN_TEST(test_parseTelegramCommand_help);
  RUN_TEST(test_parseTelegramCommand_health);
  RUN_TEST(test_parseTelegramCommand_log_bare);
  RUN_TEST(test_parseTelegramCommand_log_with_count);
  RUN_TEST(test_parseTelegramCommand_is_case_insensitive);
  RUN_TEST(test_parseTelegramCommand_on_without_target_is_picker);
  RUN_TEST(test_parseTelegramCommand_off_without_target_is_picker);
  RUN_TEST(test_parseTelegramCommand_snap_without_target_is_picker);
  RUN_TEST(test_parseTelegramCommand_on_with_trailing_space_only_is_picker);
  RUN_TEST(test_parseTelegramCommand_unrecognized_text_is_unknown);
  RUN_TEST(test_parseTelegramCommand_off_with_minutes_duration);
  RUN_TEST(test_parseTelegramCommand_on_with_clock_time_duration);
  RUN_TEST(test_parseTelegramCommand_off_without_duration_leaves_it_empty);
  RUN_TEST(test_parseTelegramCommand_off_tolerates_extra_whitespace_around_duration);
  RUN_TEST(test_parseTelegramCommand_snap_does_not_split_a_duration);
  RUN_TEST(test_parseDurationToken_plain_minutes);
  RUN_TEST(test_parseDurationToken_plain_minutes_works_without_synced_clock);
  RUN_TEST(test_parseDurationToken_rejects_zero_minutes);
  RUN_TEST(test_parseDurationToken_rejects_non_numeric_minutes);
  RUN_TEST(test_parseDurationToken_rejects_empty_token);
  RUN_TEST(test_parseDurationToken_clock_time_later_today);
  RUN_TEST(test_parseDurationToken_clock_time_already_passed_rolls_to_tomorrow);
  RUN_TEST(test_parseDurationToken_clock_time_equal_to_now_rolls_to_tomorrow);
  RUN_TEST(test_parseDurationToken_rejects_out_of_range_clock_time);
  RUN_TEST(test_parseDurationToken_rejects_malformed_clock_time);
  RUN_TEST(test_parseDurationToken_clock_time_rejected_when_unsynced);
  RUN_TEST(test_commandDisplayName_every_command);
  return UNITY_END();
}
