#include <unity.h>
#include "backoff.h"

void setUp(void) {}
void tearDown(void) {}

void test_first_failure_returns_start_delay(void) {
  TEST_ASSERT_EQUAL_UINT32(10000UL, nextBackoffDelayMs(0, 10000UL, 300000UL));
}

void test_second_failure_doubles_the_previous_delay(void) {
  TEST_ASSERT_EQUAL_UINT32(20000UL, nextBackoffDelayMs(10000UL, 10000UL, 300000UL));
}

void test_delay_is_capped_and_does_not_exceed_cap(void) {
  // 200000 * 2 = 400000, which is over the 300000 cap.
  TEST_ASSERT_EQUAL_UINT32(300000UL, nextBackoffDelayMs(200000UL, 10000UL, 300000UL));
}

void test_delay_stays_at_cap_once_reached(void) {
  TEST_ASSERT_EQUAL_UINT32(300000UL, nextBackoffDelayMs(300000UL, 10000UL, 300000UL));
}

// Documents the full sequence both main.cpp (WiFi reconnect) and
// camera.cpp (subscription retry) actually depend on: start, double,
// double, ..., cap, cap forever - and instantly back to start the moment
// the caller resets its stored delay to 0 after a success.
void test_full_sequence_matches_both_callers_expectations(void) {
  unsigned long delay = 0; // callers' initial/post-success state
  unsigned long start = 10000UL, cap = 300000UL;

  delay = nextBackoffDelayMs(delay, start, cap); TEST_ASSERT_EQUAL_UINT32(10000UL, delay);
  delay = nextBackoffDelayMs(delay, start, cap); TEST_ASSERT_EQUAL_UINT32(20000UL, delay);
  delay = nextBackoffDelayMs(delay, start, cap); TEST_ASSERT_EQUAL_UINT32(40000UL, delay);
  delay = nextBackoffDelayMs(delay, start, cap); TEST_ASSERT_EQUAL_UINT32(80000UL, delay);
  delay = nextBackoffDelayMs(delay, start, cap); TEST_ASSERT_EQUAL_UINT32(160000UL, delay);
  delay = nextBackoffDelayMs(delay, start, cap); TEST_ASSERT_EQUAL_UINT32(300000UL, delay); // 320000 -> capped
  delay = nextBackoffDelayMs(delay, start, cap); TEST_ASSERT_EQUAL_UINT32(300000UL, delay); // stays capped

  delay = 0; // simulates a success resetting the caller's stored delay
  delay = nextBackoffDelayMs(delay, start, cap);
  TEST_ASSERT_EQUAL_UINT32(10000UL, delay); // back to start, not still capped
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_first_failure_returns_start_delay);
  RUN_TEST(test_second_failure_doubles_the_previous_delay);
  RUN_TEST(test_delay_is_capped_and_does_not_exceed_cap);
  RUN_TEST(test_delay_stays_at_cap_once_reached);
  RUN_TEST(test_full_sequence_matches_both_callers_expectations);
  return UNITY_END();
}
