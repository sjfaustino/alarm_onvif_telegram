#include <unity.h>
#include <Arduino.h>
#include "format_utils.h"

void setUp(void) {}
void tearDown(void) {}

// ---- formatUptime ----

void test_formatUptime_omits_days_when_zero(void) {
  TEST_ASSERT_EQUAL_STRING("2h 5m", formatUptime(2UL * 3600UL * 1000UL + 5UL * 60UL * 1000UL).c_str());
}

void test_formatUptime_includes_days_when_nonzero(void) {
  unsigned long ms = (1UL * 86400UL + 3UL * 3600UL + 4UL * 60UL) * 1000UL;
  TEST_ASSERT_EQUAL_STRING("1d 3h 4m", formatUptime(ms).c_str());
}

void test_formatUptime_zero(void) {
  TEST_ASSERT_EQUAL_STRING("0h 0m", formatUptime(0).c_str());
}

// ---- formatElapsedSince ----

void test_formatElapsedSince_under_a_minute_is_just_now(void) {
  TEST_ASSERT_EQUAL_STRING("just now", formatElapsedSince(100000UL, 100000UL + 59000UL).c_str());
}

void test_formatElapsedSince_over_a_minute_uses_formatUptime(void) {
  unsigned long eventMs = 1000UL;
  unsigned long nowMs = eventMs + 2UL * 3600UL * 1000UL; // 2h later
  TEST_ASSERT_EQUAL_STRING("2h 0m ago", formatElapsedSince(eventMs, nowMs).c_str());
}

// ---- htmlEscape ----

void test_htmlEscape_escapes_amp_lt_gt_quot(void) {
  TEST_ASSERT_EQUAL_STRING("&amp;&lt;&gt;&quot;", htmlEscape("&<>\"").c_str());
}

// Documented, intentional: single quotes are left alone since this project
// never emits single-quoted HTML attributes.
void test_htmlEscape_leaves_single_quote_untouched(void) {
  TEST_ASSERT_EQUAL_STRING("it's fine", htmlEscape("it's fine").c_str());
}

void test_htmlEscape_leaves_plain_text_untouched(void) {
  TEST_ASSERT_EQUAL_STRING("D01-FrontDoor", htmlEscape("D01-FrontDoor").c_str());
}

// ---- urlEncode ----

void test_urlEncode_leaves_unreserved_characters_untouched(void) {
  TEST_ASSERT_EQUAL_STRING("D01-FrontDoor_v2.0~x", urlEncode("D01-FrontDoor_v2.0~x").c_str());
}

void test_urlEncode_percent_encodes_space_and_special_chars(void) {
  TEST_ASSERT_EQUAL_STRING("a%20b%26c", urlEncode("a b&c").c_str());
}

void test_urlEncode_empty_string(void) {
  TEST_ASSERT_EQUAL_STRING("", urlEncode("").c_str());
}

// ---- extractHost ----

void test_extractHost_strips_scheme_and_path(void) {
  TEST_ASSERT_EQUAL_STRING("192.168.1.50:8080", extractHost("http://192.168.1.50:8080/onvif/device_service").c_str());
}

void test_extractHost_no_path(void) {
  TEST_ASSERT_EQUAL_STRING("cam.local", extractHost("http://cam.local").c_str());
}

void test_extractHost_no_scheme(void) {
  TEST_ASSERT_EQUAL_STRING("cam.local", extractHost("cam.local/x").c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_formatUptime_omits_days_when_zero);
  RUN_TEST(test_formatUptime_includes_days_when_nonzero);
  RUN_TEST(test_formatUptime_zero);
  RUN_TEST(test_formatElapsedSince_under_a_minute_is_just_now);
  RUN_TEST(test_formatElapsedSince_over_a_minute_uses_formatUptime);
  RUN_TEST(test_htmlEscape_escapes_amp_lt_gt_quot);
  RUN_TEST(test_htmlEscape_leaves_single_quote_untouched);
  RUN_TEST(test_htmlEscape_leaves_plain_text_untouched);
  RUN_TEST(test_urlEncode_leaves_unreserved_characters_untouched);
  RUN_TEST(test_urlEncode_percent_encodes_space_and_special_chars);
  RUN_TEST(test_urlEncode_empty_string);
  RUN_TEST(test_extractHost_strips_scheme_and_path);
  RUN_TEST(test_extractHost_no_path);
  RUN_TEST(test_extractHost_no_scheme);
  return UNITY_END();
}
