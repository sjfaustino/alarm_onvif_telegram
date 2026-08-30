#include <unity.h>
#include "background_job_state.h"

void setUp(void) {}
void tearDown(void) {}

void test_fresh_state_should_start(void) {
  BackgroundJobState s;
  TEST_ASSERT_TRUE(shouldStartBackgroundJob(s));
}

void test_in_progress_state_should_not_start(void) {
  // The actual no-op-on-double-click behavior both callers (Test all
  // cameras, Search network for cameras) depend on.
  BackgroundJobState s;
  s.inProgress = true;
  TEST_ASSERT_FALSE(shouldStartBackgroundJob(s));
}

void test_markStarted_sets_inProgress(void) {
  BackgroundJobState s = markBackgroundJobStarted(BackgroundJobState{});
  TEST_ASSERT_TRUE(s.inProgress);
}

void test_markStarted_does_not_clear_a_previous_result(void) {
  // A second run starting shouldn't erase the last completed run's
  // results - status() still reports hasResult, inProgress just tells the
  // renderer to show "running" instead of them (see the header's own
  // comment on markBackgroundJobStarted).
  BackgroundJobState s;
  s.hasResult = true;
  s = markBackgroundJobStarted(s);
  TEST_ASSERT_TRUE(s.inProgress);
  TEST_ASSERT_TRUE(s.hasResult);
}

void test_markFinished_clears_inProgress_and_sets_hasResult(void) {
  BackgroundJobState s;
  s.inProgress = true;
  s = markBackgroundJobFinished(s);
  TEST_ASSERT_FALSE(s.inProgress);
  TEST_ASSERT_TRUE(s.hasResult);
}

void test_full_cycle_then_second_run_can_start_again(void) {
  BackgroundJobState s;
  TEST_ASSERT_TRUE(shouldStartBackgroundJob(s));
  s = markBackgroundJobStarted(s);
  TEST_ASSERT_FALSE(shouldStartBackgroundJob(s));
  s = markBackgroundJobFinished(s);
  TEST_ASSERT_TRUE(shouldStartBackgroundJob(s));
}

// The bug this whole transition exists to prevent: without it, a task
// creation failure right after markBackgroundJobStarted would leave
// inProgress stuck true forever, since nothing would ever call
// markBackgroundJobFinished for a task that never launched - every future
// start request silently wedged as a no-op until reboot.
void test_startFailed_clears_inProgress_so_a_retry_can_start(void) {
  BackgroundJobState s;
  s = markBackgroundJobStarted(s);
  TEST_ASSERT_FALSE(shouldStartBackgroundJob(s)); // in flight, per the caller's belief
  s = markBackgroundJobStartFailed(s); // ...but xTaskCreate actually failed
  TEST_ASSERT_TRUE(shouldStartBackgroundJob(s)); // must not be permanently wedged
  TEST_ASSERT_FALSE(s.inProgress);
}

// A failed start isn't a completed run - it must not fabricate a result,
// and must not erase a real previous one that renderers should keep
// showing until the next run actually finishes.
void test_startFailed_does_not_touch_hasResult(void) {
  BackgroundJobState fresh;
  fresh = markBackgroundJobStarted(fresh);
  fresh = markBackgroundJobStartFailed(fresh);
  TEST_ASSERT_FALSE(fresh.hasResult);

  BackgroundJobState hadResult;
  hadResult.hasResult = true;
  hadResult = markBackgroundJobStarted(hadResult);
  hadResult = markBackgroundJobStartFailed(hadResult);
  TEST_ASSERT_TRUE(hadResult.hasResult);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_state_should_start);
  RUN_TEST(test_in_progress_state_should_not_start);
  RUN_TEST(test_markStarted_sets_inProgress);
  RUN_TEST(test_markStarted_does_not_clear_a_previous_result);
  RUN_TEST(test_markFinished_clears_inProgress_and_sets_hasResult);
  RUN_TEST(test_full_cycle_then_second_run_can_start_again);
  RUN_TEST(test_startFailed_clears_inProgress_so_a_retry_can_start);
  RUN_TEST(test_startFailed_does_not_touch_hasResult);
  return UNITY_END();
}
