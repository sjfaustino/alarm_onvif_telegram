#include <unity.h>
#include "ntp_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const unsigned long HOUR = 3600000UL;
static const unsigned long RETRY = 300000UL;

void test_retries_after_a_failed_lookup(void) {
  TEST_ASSERT_TRUE(shouldRestartSntp(false, false, 0, RETRY, HOUR, RETRY));
}

void test_waits_out_the_retry_interval(void) {
  TEST_ASSERT_FALSE(shouldRestartSntp(false, false, 0, RETRY - 1, HOUR, RETRY));
}

void test_retries_when_started_but_never_synced(void) {
  TEST_ASSERT_TRUE(shouldRestartSntp(true, false, 0, RETRY, HOUR, RETRY));
}

void test_healthy_sync_is_left_alone(void) {
  TEST_ASSERT_FALSE(shouldRestartSntp(true, true, 2 * HOUR, 10 * HOUR, HOUR, RETRY));
}

void test_restarts_after_three_missed_syncs(void) {
  TEST_ASSERT_FALSE(shouldRestartSntp(true, true, 3 * HOUR, 10 * HOUR, HOUR, RETRY));
  TEST_ASSERT_TRUE(shouldRestartSntp(true, true, 3 * HOUR + 3, 10 * HOUR, HOUR, RETRY));
}

void test_long_interval_does_not_overflow(void) {
  unsigned long thirtyDays = 30UL * 24UL * HOUR;
  TEST_ASSERT_FALSE(shouldRestartSntp(true, true, 4000000000UL, 10 * HOUR, thirtyDays, RETRY));
}

void test_clock_is_set_only_after_the_threshold(void) {
  TEST_ASSERT_FALSE(clockIsSet(0));              // never synced
  TEST_ASSERT_FALSE(clockIsSet(1483228799));     // the last second of 2016
  TEST_ASSERT_TRUE(clockIsSet(1483228800));      // 2017-01-01 00:00:00 UTC
}

void test_clock_year_uses_the_same_threshold(void) {
  TEST_ASSERT_FALSE(clockYearIsSet(70));         // 1970
  TEST_ASSERT_FALSE(clockYearIsSet(116));        // 2016
  TEST_ASSERT_TRUE(clockYearIsSet(117));         // 2017
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clock_is_set_only_after_the_threshold);
  RUN_TEST(test_clock_year_uses_the_same_threshold);
  RUN_TEST(test_retries_after_a_failed_lookup);
  RUN_TEST(test_waits_out_the_retry_interval);
  RUN_TEST(test_retries_when_started_but_never_synced);
  RUN_TEST(test_healthy_sync_is_left_alone);
  RUN_TEST(test_restarts_after_three_missed_syncs);
  RUN_TEST(test_long_interval_does_not_overflow);
  return UNITY_END();
}
