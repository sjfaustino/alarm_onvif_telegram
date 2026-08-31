#include <unity.h>
#include "subscription_health.h"

void setUp(void) {}
void tearDown(void) {}

static const unsigned long THRESHOLD = 300000UL; // 5 minutes, a typical offlineThresholdMs

void test_offline_never_alerts_and_clears_flag(void) {
  // checkCameraOnlineStatus's own OFFLINE alert already covers a genuinely
  // silent camera - this must stay out of the way regardless of how long
  // it's been or whether it had already alerted before going OFFLINE.
  SubscriptionHealthResult r = evaluateSubscriptionHealth(true, THRESHOLD * 10, THRESHOLD, true);
  TEST_ASSERT_FALSE(r.alerted);
  TEST_ASSERT_FALSE(r.shouldAlert);
}

void test_recently_subscribed_does_not_alert(void) {
  SubscriptionHealthResult r = evaluateSubscriptionHealth(false, THRESHOLD - 1, THRESHOLD, false);
  TEST_ASSERT_FALSE(r.alerted);
  TEST_ASSERT_FALSE(r.shouldAlert);
}

void test_recently_subscribed_re_arms_a_previous_alert(void) {
  // Subscribed again since the last check - a fresh stretch of silence
  // later should alert again, not stay silently pre-tripped.
  SubscriptionHealthResult r = evaluateSubscriptionHealth(false, THRESHOLD - 1, THRESHOLD, true);
  TEST_ASSERT_FALSE(r.alerted);
  TEST_ASSERT_FALSE(r.shouldAlert);
}

void test_first_call_past_threshold_alerts(void) {
  SubscriptionHealthResult r = evaluateSubscriptionHealth(false, THRESHOLD, THRESHOLD, false);
  TEST_ASSERT_TRUE(r.alerted);
  TEST_ASSERT_TRUE(r.shouldAlert);
}

void test_exactly_at_threshold_alerts(void) {
  // Boundary: >= threshold, not just >, matches checkCameraOnlineStatus's
  // own offlineNow comparison (camera.cpp/telegram.cpp).
  SubscriptionHealthResult r = evaluateSubscriptionHealth(false, THRESHOLD, THRESHOLD, false);
  TEST_ASSERT_TRUE(r.shouldAlert);
}

void test_already_alerted_does_not_alert_again(void) {
  // Same stretch of unsubscribed time, second (or later) call - one alert
  // per stretch, not a repeat every cameraTaskFn loop pass.
  SubscriptionHealthResult r = evaluateSubscriptionHealth(false, THRESHOLD * 3, THRESHOLD, true);
  TEST_ASSERT_TRUE(r.alerted);
  TEST_ASSERT_FALSE(r.shouldAlert);
}

void test_full_cycle_alert_then_recover_then_alert_again(void) {
  bool alerted = false;

  SubscriptionHealthResult r1 = evaluateSubscriptionHealth(false, THRESHOLD, THRESHOLD, alerted);
  alerted = r1.alerted;
  TEST_ASSERT_TRUE(r1.shouldAlert);

  SubscriptionHealthResult r2 = evaluateSubscriptionHealth(false, THRESHOLD + 1000, THRESHOLD, alerted);
  alerted = r2.alerted;
  TEST_ASSERT_FALSE(r2.shouldAlert); // still the same stretch - no repeat

  SubscriptionHealthResult r3 = evaluateSubscriptionHealth(false, 0, THRESHOLD, alerted); // subscribed again
  alerted = r3.alerted;
  TEST_ASSERT_FALSE(r3.shouldAlert);
  TEST_ASSERT_FALSE(alerted);

  SubscriptionHealthResult r4 = evaluateSubscriptionHealth(false, THRESHOLD, THRESHOLD, alerted);
  TEST_ASSERT_TRUE(r4.shouldAlert); // a fresh stretch - alerts again
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_offline_never_alerts_and_clears_flag);
  RUN_TEST(test_recently_subscribed_does_not_alert);
  RUN_TEST(test_recently_subscribed_re_arms_a_previous_alert);
  RUN_TEST(test_first_call_past_threshold_alerts);
  RUN_TEST(test_exactly_at_threshold_alerts);
  RUN_TEST(test_already_alerted_does_not_alert_again);
  RUN_TEST(test_full_cycle_alert_then_recover_then_alert_again);
  return UNITY_END();
}
