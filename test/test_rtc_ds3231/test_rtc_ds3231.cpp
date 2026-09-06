#include <unity.h>
#include <Arduino.h>
#include "rtc_ds3231.h"

void setUp(void) {}
void tearDown(void) {}

static struct tm makeTm(int year, int mon0, int mday, int hour, int min, int sec, int wday) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = mon0;
  t.tm_mday = mday;
  t.tm_hour = hour;
  t.tm_min = min;
  t.tm_sec = sec;
  t.tm_wday = wday;
  return t;
}

static void assertTmEqual(const struct tm& expected, const struct tm& actual) {
  TEST_ASSERT_EQUAL_INT(expected.tm_year, actual.tm_year);
  TEST_ASSERT_EQUAL_INT(expected.tm_mon, actual.tm_mon);
  TEST_ASSERT_EQUAL_INT(expected.tm_mday, actual.tm_mday);
  TEST_ASSERT_EQUAL_INT(expected.tm_hour, actual.tm_hour);
  TEST_ASSERT_EQUAL_INT(expected.tm_min, actual.tm_min);
  TEST_ASSERT_EQUAL_INT(expected.tm_sec, actual.tm_sec);
  TEST_ASSERT_EQUAL_INT(expected.tm_wday, actual.tm_wday);
}

// ---- Round trip ----

void test_round_trip_ordinary_date(void) {
  // 2026-09-06 14:30:22, a Sunday (tm_wday=0).
  struct tm t = makeTm(2026, 8, 6, 14, 30, 22, 0);
  struct tm restored = decodeDs3231(encodeDs3231(t));
  assertTmEqual(t, restored);
}

void test_round_trip_leap_day(void) {
  // 2028-02-29 00:00:00, a Tuesday (tm_wday=2) - 2028 is a real leap year
  // (divisible by 4, not by 100).
  struct tm t = makeTm(2028, 1, 29, 0, 0, 0, 2);
  struct tm restored = decodeDs3231(encodeDs3231(t));
  assertTmEqual(t, restored);
}

void test_round_trip_last_second_of_day(void) {
  // 23:59:59 - the boundary the hours/minutes/seconds BCD encoding must
  // still get exactly right, not just typical mid-day values.
  struct tm t = makeTm(2026, 11, 31, 23, 59, 59, 4);
  struct tm restored = decodeDs3231(encodeDs3231(t));
  assertTmEqual(t, restored);
}

void test_round_trip_midnight_and_january(void) {
  struct tm t = makeTm(2026, 0, 1, 0, 0, 0, 4);
  struct tm restored = decodeDs3231(encodeDs3231(t));
  assertTmEqual(t, restored);
}

// ---- Field-level encoding checks ----

// The whole reason this project never needs to handle 12-hour mode on
// read: encode always writes 24-hour mode (bit 6 of the hours register
// clear), regardless of the hour value - checked across the low, middle,
// and high end of the 0-23 range.
void test_encode_always_clears_12_24_hour_bit(void) {
  for (int hour : {0, 12, 23}) {
    struct tm t = makeTm(2026, 0, 1, hour, 0, 0, 0);
    Ds3231Registers regs = encodeDs3231(t);
    TEST_ASSERT_EQUAL_UINT8(0, regs.bytes[2] & 0x40);
  }
}

void test_encode_year_2026_maps_to_bcd_26(void) {
  struct tm t = makeTm(2026, 0, 1, 0, 0, 0, 0);
  Ds3231Registers regs = encodeDs3231(t);
  TEST_ASSERT_EQUAL_HEX8(0x26, regs.bytes[6]);
}

void test_encode_january_maps_to_month_register_1(void) {
  struct tm t = makeTm(2026, 0, 1, 0, 0, 0, 0);
  Ds3231Registers regs = encodeDs3231(t);
  TEST_ASSERT_EQUAL_HEX8(0x01, regs.bytes[5] & 0x1F);
}

void test_encode_december_maps_to_month_register_12(void) {
  struct tm t = makeTm(2026, 11, 1, 0, 0, 0, 0);
  Ds3231Registers regs = encodeDs3231(t);
  TEST_ASSERT_EQUAL_HEX8(0x12, regs.bytes[5] & 0x1F); // BCD 12, not decimal 12
}

// tm_wday is 0-6 (Sunday-Saturday); the DS3231's day-of-week register wants
// 1-7 - Sunday must map to 1, Saturday to 7, and back again, not off by one
// in either direction.
void test_dayOfWeek_sunday_maps_to_register_1_and_back(void) {
  struct tm t = makeTm(2026, 0, 4, 0, 0, 0, 0); // a Sunday
  Ds3231Registers regs = encodeDs3231(t);
  TEST_ASSERT_EQUAL_UINT8(1, regs.bytes[3]);
  TEST_ASSERT_EQUAL_INT(0, decodeDs3231(regs).tm_wday);
}

void test_dayOfWeek_saturday_maps_to_register_7_and_back(void) {
  struct tm t = makeTm(2026, 0, 10, 0, 0, 0, 6); // a Saturday
  Ds3231Registers regs = encodeDs3231(t);
  TEST_ASSERT_EQUAL_UINT8(7, regs.bytes[3]);
  TEST_ASSERT_EQUAL_INT(6, decodeDs3231(regs).tm_wday);
}

// ---- decode defensiveness ----

// decodeDs3231 must mask off the hours register's 12/24 and AM/PM bits
// rather than letting them corrupt the decoded hour - simulates a chip
// that (for whatever reason) has its 12-hour-mode bit set, confirming the
// masked-out hour value (bits 3-0, the ones digit BCD) still comes back
// as a plain, in-range number rather than something garbled by the
// high bits.
void test_decode_masks_off_12_24_and_am_pm_bits(void) {
  Ds3231Registers regs;
  regs.bytes[2] = 0x40 | 0x09; // 12/24 bit set + BCD 09 in the low bits
  struct tm result = decodeDs3231(regs);
  TEST_ASSERT_EQUAL_INT(9, result.tm_hour);
}

// ---- timeGmUtc ----
//
// Checked against well-known reference epoch values, not against the host
// machine's own mktime()/timegm() - the whole point of this function is to
// not depend on libc's timezone handling at all, so the test shouldn't
// either.

void test_timeGmUtc_unix_epoch(void) {
  struct tm t = makeTm(1970, 0, 1, 0, 0, 0, 4);
  TEST_ASSERT_EQUAL_INT64(0, (int64_t)timeGmUtc(t));
}

void test_timeGmUtc_known_reference_date(void) {
  // 2026-01-01 00:00:00 UTC = 1767225600 (a standard, independently
  // verifiable reference value for this date).
  struct tm t = makeTm(2026, 0, 1, 0, 0, 0, 4);
  TEST_ASSERT_EQUAL_INT64(1767225600LL, (int64_t)timeGmUtc(t));
}

void test_timeGmUtc_accounts_for_leap_years(void) {
  // 2028-03-01 00:00:00 UTC must be exactly 60 days (Jan 31 + Feb 29, a
  // leap year) after 2028-01-01 00:00:00 UTC - a leap-year bug would be
  // off by one day here.
  struct tm jan1 = makeTm(2028, 0, 1, 0, 0, 0, 6);
  struct tm mar1 = makeTm(2028, 2, 1, 0, 0, 0, 3);
  time_t diffSeconds = timeGmUtc(mar1) - timeGmUtc(jan1);
  TEST_ASSERT_EQUAL_INT64(60L * 86400L, (int64_t)diffSeconds);
}

void test_timeGmUtc_time_of_day_offset(void) {
  struct tm midnight = makeTm(2026, 5, 15, 0, 0, 0, 1);
  struct tm later = makeTm(2026, 5, 15, 14, 30, 22, 1);
  time_t diffSeconds = timeGmUtc(later) - timeGmUtc(midnight);
  TEST_ASSERT_EQUAL_INT64(14L * 3600L + 30L * 60L + 22L, (int64_t)diffSeconds);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_ordinary_date);
  RUN_TEST(test_round_trip_leap_day);
  RUN_TEST(test_round_trip_last_second_of_day);
  RUN_TEST(test_round_trip_midnight_and_january);
  RUN_TEST(test_encode_always_clears_12_24_hour_bit);
  RUN_TEST(test_encode_year_2026_maps_to_bcd_26);
  RUN_TEST(test_encode_january_maps_to_month_register_1);
  RUN_TEST(test_encode_december_maps_to_month_register_12);
  RUN_TEST(test_dayOfWeek_sunday_maps_to_register_1_and_back);
  RUN_TEST(test_dayOfWeek_saturday_maps_to_register_7_and_back);
  RUN_TEST(test_decode_masks_off_12_24_and_am_pm_bits);
  RUN_TEST(test_timeGmUtc_unix_epoch);
  RUN_TEST(test_timeGmUtc_known_reference_date);
  RUN_TEST(test_timeGmUtc_accounts_for_leap_years);
  RUN_TEST(test_timeGmUtc_time_of_day_offset);
  return UNITY_END();
}
