#include <unity.h>
#include "task_stall.h"

void setUp(void) {}
void tearDown(void) {}

static const unsigned long THRESHOLD = 600000UL;

void test_healthy_task_does_nothing(void) {
  TaskStallResult r = evaluateTaskStall(true, 1000, THRESHOLD, false);
  TEST_ASSERT_FALSE(r.shouldAlert);
  TEST_ASSERT_FALSE(r.recovered);
  TEST_ASSERT_FALSE(r.alerted);
}

void test_alerts_when_threshold_reached(void) {
  TaskStallResult r = evaluateTaskStall(true, THRESHOLD, THRESHOLD, false);
  TEST_ASSERT_TRUE(r.shouldAlert);
  TEST_ASSERT_TRUE(r.alerted);
}

void test_just_under_threshold_does_not_alert(void) {
  TEST_ASSERT_FALSE(evaluateTaskStall(true, THRESHOLD - 1, THRESHOLD, false).shouldAlert);
}

void test_alerts_only_once_per_stall(void) {
  TaskStallResult r = evaluateTaskStall(true, THRESHOLD * 2, THRESHOLD, true);
  TEST_ASSERT_FALSE(r.shouldAlert);
  TEST_ASSERT_FALSE(r.recovered);
  TEST_ASSERT_TRUE(r.alerted);
}

void test_recovery_reports_once_and_rearms(void) {
  TaskStallResult r = evaluateTaskStall(true, 500, THRESHOLD, true);
  TEST_ASSERT_TRUE(r.recovered);
  TEST_ASSERT_FALSE(r.alerted);
  TEST_ASSERT_FALSE(evaluateTaskStall(true, 500, THRESHOLD, r.alerted).recovered);
  TEST_ASSERT_TRUE(evaluateTaskStall(true, THRESHOLD, THRESHOLD, r.alerted).shouldAlert);
}

void test_task_that_exited_is_not_a_stall(void) {
  TEST_ASSERT_FALSE(evaluateTaskStall(false, THRESHOLD * 10, THRESHOLD, false).shouldAlert);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_healthy_task_does_nothing);
  RUN_TEST(test_alerts_when_threshold_reached);
  RUN_TEST(test_just_under_threshold_does_not_alert);
  RUN_TEST(test_alerts_only_once_per_stall);
  RUN_TEST(test_recovery_reports_once_and_rearms);
  RUN_TEST(test_task_that_exited_is_not_a_stall);
  return UNITY_END();
}
