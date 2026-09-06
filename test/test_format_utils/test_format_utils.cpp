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

// The exact 60000ms threshold itself, not just comfortably below/above it
// (the existing tests use 59000 and 2h) - a `<` -> `<=` mutation would
// silently make a genuinely one-minute-old event still read "just now"
// instead of "0h 1m ago", and neither of those other tests would catch it.
void test_formatElapsedSince_exactly_one_minute_is_not_just_now(void) {
  TEST_ASSERT_EQUAL_STRING("0h 1m ago", formatElapsedSince(100000UL, 100000UL + 60000UL).c_str());
}

// ---- htmlEscape ----

void test_htmlEscape_escapes_amp_lt_gt_quot(void) {
  TEST_ASSERT_EQUAL_STRING("&amp;&lt;&gt;&quot;", htmlEscape("&<>\"").c_str());
}

// A single quote must be escaped too, even though this project never
// emits single-quoted HTML *attributes* - renderEditDeleteActions
// (lib/webserver_html) interpolates an htmlEscape()d name into a
// single-quoted JS string INSIDE a double-quoted onsubmit="..." attribute
// (onsubmit="return confirm('...')"). The attribute itself being double-
// quoted doesn't protect the JS string literal nested inside it - an
// unescaped ' there breaks out of that string and lets the rest of the
// name execute as script. This was a real, confirmed XSS gap, not a
// hypothetical - a previous version of this test asserted the opposite
// (unescaped) as "intentional," which is exactly how it went unnoticed.
void test_htmlEscape_escapes_single_quote(void) {
  TEST_ASSERT_EQUAL_STRING("it&#39;s fine", htmlEscape("it's fine").c_str());
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

// ---- jsSingleQuoteEscape ----

void test_jsSingleQuoteEscape_leaves_plain_text_untouched(void) {
  TEST_ASSERT_EQUAL_STRING("192.168.1.1", jsSingleQuoteEscape("192.168.1.1").c_str());
}

// The actual vulnerability this exists to close: an unescaped single quote
// would terminate the surrounding 'VALUE' JS string literal early, letting
// whatever follows run as arbitrary script.
void test_jsSingleQuoteEscape_escapes_single_quote(void) {
  String escaped = jsSingleQuoteEscape("x'};alert(1);var y={z:'");
  TEST_ASSERT_TRUE(escaped.indexOf("\\'") >= 0);
  TEST_ASSERT_TRUE(escaped.indexOf("x\\'}") >= 0); // the quote right after x is escaped, not left raw
}

// Backslash must be escaped too, and specifically BEFORE the single-quote
// pass runs - otherwise a raw backslash sitting just before a quote this
// function itself inserts (\') would combine into \\' , which a JS parser
// reads as an escaped backslash followed by an UNescaped, string-
// terminating quote - reintroducing the exact breakout this function
// exists to prevent.
void test_jsSingleQuoteEscape_escapes_backslash_before_quote_pass(void) {
  String escaped = jsSingleQuoteEscape("a\\'b");
  TEST_ASSERT_EQUAL_STRING("a\\\\\\'b", escaped.c_str());
}

void test_jsSingleQuoteEscape_escapes_newlines(void) {
  String escaped = jsSingleQuoteEscape("line1\nline2");
  TEST_ASSERT_TRUE(escaped.indexOf('\n') < 0); // no raw newline left in the output
  TEST_ASSERT_TRUE(escaped.indexOf("\\n") >= 0);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_formatUptime_omits_days_when_zero);
  RUN_TEST(test_formatUptime_includes_days_when_nonzero);
  RUN_TEST(test_formatUptime_zero);
  RUN_TEST(test_formatElapsedSince_under_a_minute_is_just_now);
  RUN_TEST(test_formatElapsedSince_over_a_minute_uses_formatUptime);
  RUN_TEST(test_formatElapsedSince_exactly_one_minute_is_not_just_now);
  RUN_TEST(test_htmlEscape_escapes_amp_lt_gt_quot);
  RUN_TEST(test_htmlEscape_escapes_single_quote);
  RUN_TEST(test_htmlEscape_leaves_plain_text_untouched);
  RUN_TEST(test_urlEncode_leaves_unreserved_characters_untouched);
  RUN_TEST(test_urlEncode_percent_encodes_space_and_special_chars);
  RUN_TEST(test_urlEncode_empty_string);
  RUN_TEST(test_extractHost_strips_scheme_and_path);
  RUN_TEST(test_extractHost_no_path);
  RUN_TEST(test_extractHost_no_scheme);
  RUN_TEST(test_jsSingleQuoteEscape_leaves_plain_text_untouched);
  RUN_TEST(test_jsSingleQuoteEscape_escapes_single_quote);
  RUN_TEST(test_jsSingleQuoteEscape_escapes_backslash_before_quote_pass);
  RUN_TEST(test_jsSingleQuoteEscape_escapes_newlines);
  return UNITY_END();
}
