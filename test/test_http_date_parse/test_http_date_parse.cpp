#include <unity.h>
#include <Arduino.h>
#include "http_date_parse.h"

void setUp(void) {}
void tearDown(void) {}

void test_parseHttpDate_valid_imf_fixdate(void) {
  struct tm tmOut;
  bool ok = parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT", tmOut);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_INT(6, tmOut.tm_mday);
  TEST_ASSERT_EQUAL_INT(10, tmOut.tm_mon); // November = index 10
  TEST_ASSERT_EQUAL_INT(94, tmOut.tm_year); // 1994 - 1900
  TEST_ASSERT_EQUAL_INT(8, tmOut.tm_hour);
  TEST_ASSERT_EQUAL_INT(49, tmOut.tm_min);
  TEST_ASSERT_EQUAL_INT(37, tmOut.tm_sec);
}

void test_parseHttpDate_every_month_abbreviation(void) {
  const char* dates[12] = {
    "Mon, 01 Jan 2024 00:00:00 GMT", "Mon, 01 Feb 2024 00:00:00 GMT", "Mon, 01 Mar 2024 00:00:00 GMT",
    "Mon, 01 Apr 2024 00:00:00 GMT", "Mon, 01 May 2024 00:00:00 GMT", "Mon, 01 Jun 2024 00:00:00 GMT",
    "Mon, 01 Jul 2024 00:00:00 GMT", "Mon, 01 Aug 2024 00:00:00 GMT", "Mon, 01 Sep 2024 00:00:00 GMT",
    "Mon, 01 Oct 2024 00:00:00 GMT", "Mon, 01 Nov 2024 00:00:00 GMT", "Mon, 01 Dec 2024 00:00:00 GMT"};
  for (int m = 0; m < 12; m++) {
    struct tm tmOut;
    TEST_ASSERT_TRUE(parseHttpDate(String(dates[m]), tmOut));
    TEST_ASSERT_EQUAL_INT(m, tmOut.tm_mon);
  }
}

void test_parseHttpDate_wrong_length_rejected(void) {
  struct tm tmOut;
  TEST_ASSERT_FALSE(parseHttpDate("06 Nov 1994 08:49:37 GMT", tmOut)); // missing day name
  TEST_ASSERT_FALSE(parseHttpDate("", tmOut));
}

void test_parseHttpDate_wrong_delimiters_rejected(void) {
  struct tm tmOut;
  TEST_ASSERT_FALSE(parseHttpDate("Sun; 06 Nov 1994 08:49:37 GMT", tmOut)); // ';' not ','
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 06-Nov-1994 08:49:37 GMT", tmOut)); // '-' not ' '
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 06 Nov 1994 08.49.37 GMT", tmOut)); // '.' not ':'
}

void test_parseHttpDate_wrong_timezone_rejected(void) {
  struct tm tmOut;
  // Same length as a real header, but not GMT - a legacy asctime-with-offset
  // or a non-UTC zone must not be silently treated as UTC.
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 06 Nov 1994 08:49:37 UTC", tmOut));
}

void test_parseHttpDate_unrecognized_month_rejected(void) {
  struct tm tmOut;
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 06 Foo 1994 08:49:37 GMT", tmOut));
}

void test_parseHttpDate_non_numeric_fields_rejected(void) {
  struct tm tmOut;
  TEST_ASSERT_FALSE(parseHttpDate("Sun, XX Nov 1994 08:49:37 GMT", tmOut)); // day
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 06 Nov XXXX 08:49:37 GMT", tmOut)); // year
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 06 Nov 1994 XX:49:37 GMT", tmOut)); // hour
}

// A `String::toInt()` on non-numeric input silently returns 0 - without
// allDigits()'s explicit check, "Sun, XX Nov 1994 08:49:37 GMT" would
// parse as day 0 (still rejected by the day<1 range check) but a subtler
// non-numeric value that happens to look range-valid would slip through
// as a wrong, not-obviously-wrong time. This pins that the day field
// specifically is validated as digits, not just range-checked after the
// fact.
void test_parseHttpDate_out_of_range_values_rejected(void) {
  struct tm tmOut;
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 00 Nov 1994 08:49:37 GMT", tmOut)); // day 0
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 06 Nov 1994 24:49:37 GMT", tmOut)); // hour 24
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 06 Nov 1994 08:60:37 GMT", tmOut)); // minute 60
  TEST_ASSERT_FALSE(parseHttpDate("Sun, 06 Nov 1994 08:49:61 GMT", tmOut)); // second 61
}

void test_parseHttpDate_leap_second_tolerated(void) {
  struct tm tmOut;
  TEST_ASSERT_TRUE(parseHttpDate("Sun, 06 Nov 1994 08:49:60 GMT", tmOut));
  TEST_ASSERT_EQUAL_INT(60, tmOut.tm_sec);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_parseHttpDate_valid_imf_fixdate);
  RUN_TEST(test_parseHttpDate_every_month_abbreviation);
  RUN_TEST(test_parseHttpDate_wrong_length_rejected);
  RUN_TEST(test_parseHttpDate_wrong_delimiters_rejected);
  RUN_TEST(test_parseHttpDate_wrong_timezone_rejected);
  RUN_TEST(test_parseHttpDate_unrecognized_month_rejected);
  RUN_TEST(test_parseHttpDate_non_numeric_fields_rejected);
  RUN_TEST(test_parseHttpDate_out_of_range_values_rejected);
  RUN_TEST(test_parseHttpDate_leap_second_tolerated);
  return UNITY_END();
}
