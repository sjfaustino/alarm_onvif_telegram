#include <unity.h>
#include "quiet_hours.h"

void setUp(void) {}
void tearDown(void) {}

void test_start_equal_end_is_never_quiet(void) {
  // The natural pre-filled 00:00/00:00 state - must NOT mean "always
  // quiet" (see quiet_hours.h's own comment for why).
  TEST_ASSERT_FALSE(isWithinQuietHours(0, 0, 0));
  TEST_ASSERT_FALSE(isWithinQuietHours(720, 540, 540));
}

void test_same_day_window_inside(void) {
  // 09:00 (540) - 17:00 (1020)
  TEST_ASSERT_TRUE(isWithinQuietHours(540, 540, 1020));   // exactly at start - inclusive
  TEST_ASSERT_TRUE(isWithinQuietHours(700, 540, 1020));   // well inside
  TEST_ASSERT_TRUE(isWithinQuietHours(1019, 540, 1020));  // one minute before end
}

void test_same_day_window_outside(void) {
  TEST_ASSERT_FALSE(isWithinQuietHours(1020, 540, 1020)); // exactly at end - exclusive
  TEST_ASSERT_FALSE(isWithinQuietHours(539, 540, 1020));  // one minute before start
  TEST_ASSERT_FALSE(isWithinQuietHours(0, 540, 1020));    // well outside
}

void test_overnight_window_inside(void) {
  // 22:00 (1320) - 06:00 (360), wraps past midnight
  TEST_ASSERT_TRUE(isWithinQuietHours(1320, 1320, 360));  // exactly at start
  TEST_ASSERT_TRUE(isWithinQuietHours(1439, 1320, 360));  // late night
  TEST_ASSERT_TRUE(isWithinQuietHours(0, 1320, 360));     // midnight
  TEST_ASSERT_TRUE(isWithinQuietHours(359, 1320, 360));   // one minute before end
}

void test_overnight_window_outside(void) {
  TEST_ASSERT_FALSE(isWithinQuietHours(360, 1320, 360));  // exactly at end - exclusive
  TEST_ASSERT_FALSE(isWithinQuietHours(700, 1320, 360));  // mid-afternoon
  TEST_ASSERT_FALSE(isWithinQuietHours(1319, 1320, 360)); // one minute before start
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_start_equal_end_is_never_quiet);
  RUN_TEST(test_same_day_window_inside);
  RUN_TEST(test_same_day_window_outside);
  RUN_TEST(test_overnight_window_inside);
  RUN_TEST(test_overnight_window_outside);
  return UNITY_END();
}
