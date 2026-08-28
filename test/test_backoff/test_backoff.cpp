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

// unsigned long is 32-bit on this platform - previousDelayMs above
// ULONG_MAX/2 makes the internal *2UL wrap to a small value. Without the
// overflow guard, that wrapped value could read as "under capMs" and get
// returned directly instead of the cap - the delay would shrink instead
// of staying clamped. No real caller reaches this today, but this is a
// shared primitive; verifies the guard, not just the reachable range.
void test_delay_does_not_shrink_on_overflow(void) {
  TEST_ASSERT_EQUAL_UINT32(300000UL, nextBackoffDelayMs(3000000000UL, 10000UL, 300000UL));
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

// ---- detectorSafeBackoffCapMs ----
// The exact bug hit in the field: camera.cpp's retry backoff ceiling and
// its offline-threshold alert were both independently 5 minutes, so a
// camera whose backoff reached its ceiling could go quiet just long
// enough between retries to false-trip "OFFLINE" on its own.

void test_detectorSafeBackoffCapMs_half_the_threshold_when_under_global_cap(void) {
  // 5 min threshold / 2 = 2.5 min, well under the 5 min global cap.
  TEST_ASSERT_EQUAL_UINT32(150000UL, detectorSafeBackoffCapMs(300000UL, 300000UL, 10000UL));
}

// This is the actual field scenario: threshold and global cap both 5
// minutes - half the threshold (2.5 min) must win, not the global cap.
void test_detectorSafeBackoffCapMs_threshold_equal_to_global_cap(void) {
  TEST_ASSERT_EQUAL_UINT32(150000UL, detectorSafeBackoffCapMs(300000UL, 300000UL, 10000UL));
}

// A generous offline threshold (e.g. 30 minutes) shouldn't let the retry
// backoff grow past its own intended global ceiling.
void test_detectorSafeBackoffCapMs_global_cap_wins_when_threshold_is_generous(void) {
  TEST_ASSERT_EQUAL_UINT32(300000UL, detectorSafeBackoffCapMs(300000UL, 1800000UL, 10000UL));
}

// A very short configured threshold must not push the cap below the
// backoff's own first-attempt delay - the result is never less than startMs.
void test_detectorSafeBackoffCapMs_floored_at_startMs(void) {
  TEST_ASSERT_EQUAL_UINT32(10000UL, detectorSafeBackoffCapMs(300000UL, 60UL, 10000UL));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_first_failure_returns_start_delay);
  RUN_TEST(test_second_failure_doubles_the_previous_delay);
  RUN_TEST(test_delay_is_capped_and_does_not_exceed_cap);
  RUN_TEST(test_delay_stays_at_cap_once_reached);
  RUN_TEST(test_delay_does_not_shrink_on_overflow);
  RUN_TEST(test_full_sequence_matches_both_callers_expectations);
  RUN_TEST(test_detectorSafeBackoffCapMs_half_the_threshold_when_under_global_cap);
  RUN_TEST(test_detectorSafeBackoffCapMs_threshold_equal_to_global_cap);
  RUN_TEST(test_detectorSafeBackoffCapMs_global_cap_wins_when_threshold_is_generous);
  RUN_TEST(test_detectorSafeBackoffCapMs_floored_at_startMs);
  return UNITY_END();
}
