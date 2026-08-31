#include <unity.h>
#include "heap_health.h"

void setUp(void) {}
void tearDown(void) {}

static const uint32_t WARN = 20000UL;

void test_first_call_baselines_and_logs_but_is_not_a_new_low(void) {
  HeapHealthResult r = evaluateHeapHealth(163540UL, false, 0, WARN, false);
  TEST_ASSERT_EQUAL_UINT32(163540UL, r.baseline);
  TEST_ASSERT_TRUE(r.shouldLog);
  TEST_ASSERT_FALSE(r.isNewLow);
  TEST_ASSERT_FALSE(r.shouldAlert);
}

void test_first_call_already_below_threshold_alerts(void) {
  // A board that was already in trouble before this firmware update even
  // ran once - still worth the heads-up immediately, not only on some
  // later drop.
  HeapHealthResult r = evaluateHeapHealth(732UL, false, 0, WARN, false);
  TEST_ASSERT_TRUE(r.shouldLog);
  TEST_ASSERT_FALSE(r.isNewLow);
  TEST_ASSERT_TRUE(r.shouldAlert);
}

void test_unchanged_value_does_not_log(void) {
  HeapHealthResult r = evaluateHeapHealth(163540UL, true, 163540UL, WARN, false);
  TEST_ASSERT_FALSE(r.shouldLog);
  TEST_ASSERT_FALSE(r.isNewLow);
  TEST_ASSERT_FALSE(r.shouldAlert);
  TEST_ASSERT_EQUAL_UINT32(163540UL, r.baseline); // unchanged
}

void test_higher_value_does_not_log(void) {
  // Free heap recovering (e.g. a TLS session torn down) never counts as a
  // "new low" - getMinFreeHeap() itself can only ever decrease within a
  // boot, but defensive here regardless.
  HeapHealthResult r = evaluateHeapHealth(200000UL, true, 163540UL, WARN, false);
  TEST_ASSERT_FALSE(r.shouldLog);
  TEST_ASSERT_EQUAL_UINT32(163540UL, r.baseline);
}

void test_new_low_above_threshold_logs_but_does_not_alert(void) {
  HeapHealthResult r = evaluateHeapHealth(150000UL, true, 163540UL, WARN, false);
  TEST_ASSERT_TRUE(r.shouldLog);
  TEST_ASSERT_TRUE(r.isNewLow);
  TEST_ASSERT_FALSE(r.shouldAlert);
  TEST_ASSERT_EQUAL_UINT32(150000UL, r.baseline);
}

void test_new_low_crossing_threshold_alerts_once(void) {
  HeapHealthResult r = evaluateHeapHealth(15000UL, true, 25000UL, WARN, false);
  TEST_ASSERT_TRUE(r.shouldLog);
  TEST_ASSERT_TRUE(r.isNewLow);
  TEST_ASSERT_TRUE(r.shouldAlert);
}

void test_further_drop_after_already_alerted_logs_but_does_not_re_alert(void) {
  // Still a genuinely new low worth logging (the trail keeps growing) but
  // the Telegram alert doesn't repeat every single further decrement.
  HeapHealthResult r = evaluateHeapHealth(500UL, true, 15000UL, WARN, true);
  TEST_ASSERT_TRUE(r.shouldLog);
  TEST_ASSERT_TRUE(r.isNewLow);
  TEST_ASSERT_FALSE(r.shouldAlert);
}

void test_full_sequence(void) {
  bool hasBaseline = false;
  uint32_t baseline = 0;
  bool alerted = false;

  HeapHealthResult r1 = evaluateHeapHealth(163540UL, hasBaseline, baseline, WARN, alerted);
  hasBaseline = true; baseline = r1.baseline; alerted = alerted || r1.shouldAlert;
  TEST_ASSERT_FALSE(r1.shouldAlert);

  HeapHealthResult r2 = evaluateHeapHealth(163540UL, hasBaseline, baseline, WARN, alerted); // no change
  baseline = r2.baseline;
  TEST_ASSERT_FALSE(r2.shouldLog);

  HeapHealthResult r3 = evaluateHeapHealth(25000UL, hasBaseline, baseline, WARN, alerted); // drops, still above warn
  baseline = r3.baseline;
  TEST_ASSERT_TRUE(r3.shouldLog);
  TEST_ASSERT_FALSE(r3.shouldAlert);

  HeapHealthResult r4 = evaluateHeapHealth(732UL, hasBaseline, baseline, WARN, alerted); // crosses warn
  baseline = r4.baseline; alerted = alerted || r4.shouldAlert;
  TEST_ASSERT_TRUE(r4.shouldAlert);
  TEST_ASSERT_TRUE(alerted);

  HeapHealthResult r5 = evaluateHeapHealth(100UL, hasBaseline, baseline, WARN, alerted); // even lower
  TEST_ASSERT_TRUE(r5.shouldLog);
  TEST_ASSERT_FALSE(r5.shouldAlert); // already alerted this boot
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_first_call_baselines_and_logs_but_is_not_a_new_low);
  RUN_TEST(test_first_call_already_below_threshold_alerts);
  RUN_TEST(test_unchanged_value_does_not_log);
  RUN_TEST(test_higher_value_does_not_log);
  RUN_TEST(test_new_low_above_threshold_logs_but_does_not_alert);
  RUN_TEST(test_new_low_crossing_threshold_alerts_once);
  RUN_TEST(test_further_drop_after_already_alerted_logs_but_does_not_re_alert);
  RUN_TEST(test_full_sequence);
  return UNITY_END();
}
