#include <unity.h>
#include "camera_clamps.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

void test_cooldown_zero_uses_default_not_no_throttling(void) {
  CameraConfig c;
  c.alertCooldownMs = 0;
  TEST_ASSERT_EQUAL_UINT32(CameraConfig().alertCooldownMs, safeAlertCooldownMs(c));
}

void test_cooldown_is_capped_and_normal_values_pass(void) {
  CameraConfig c;
  c.alertCooldownMs = CAMERA_ALERT_COOLDOWN_MAX_MS + 1;
  TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_COOLDOWN_MAX_MS, safeAlertCooldownMs(c));
  c.alertCooldownMs = 45000;
  TEST_ASSERT_EQUAL_UINT32(45000, safeAlertCooldownMs(c));
}

void test_offline_threshold_zero_uses_default(void) {
  CameraConfig c;
  c.offlineThresholdMs = 0;
  TEST_ASSERT_EQUAL_UINT32(CameraConfig().offlineThresholdMs, safeOfflineThresholdMs(c));
}

void test_offline_threshold_is_capped(void) {
  CameraConfig c;
  c.offlineThresholdMs = CAMERA_OFFLINE_THRESHOLD_MAX_MS + 1;
  TEST_ASSERT_EQUAL_UINT32(CAMERA_OFFLINE_THRESHOLD_MAX_MS, safeOfflineThresholdMs(c));
}

void test_burst_count_range(void) {
  CameraConfig c;
  c.snapshotBurstCount = 0;
  TEST_ASSERT_EQUAL_UINT(1, safeSnapshotBurstCount(c));
  c.snapshotBurstCount = CAMERA_SNAPSHOT_BURST_MAX + 1;
  TEST_ASSERT_EQUAL_UINT(CAMERA_SNAPSHOT_BURST_MAX, safeSnapshotBurstCount(c));
  c.snapshotBurstCount = 3;
  TEST_ASSERT_EQUAL_UINT(3, safeSnapshotBurstCount(c));
}

void test_snapshot_dimension_zero_stays_unset(void) {
  TEST_ASSERT_EQUAL_UINT16(0, safeSnapshotDimension(0));
  TEST_ASSERT_EQUAL_UINT16(640, safeSnapshotDimension(640));
  TEST_ASSERT_EQUAL_UINT16(CAMERA_SNAPSHOT_DIMENSION_MAX, safeSnapshotDimension(65535));
}

void test_poll_interval_range(void) {
  CameraConfig c;
  c.pollIntervalMs = 0;
  TEST_ASSERT_EQUAL_UINT32(CAMERA_POLL_INTERVAL_MIN_MS, safePollIntervalMs(c));
  c.pollIntervalMs = CAMERA_POLL_INTERVAL_MAX_MS + 1;
  TEST_ASSERT_EQUAL_UINT32(CAMERA_POLL_INTERVAL_MAX_MS, safePollIntervalMs(c));
  c.pollIntervalMs = 1500;
  TEST_ASSERT_EQUAL_UINT32(1500, safePollIntervalMs(c));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cooldown_zero_uses_default_not_no_throttling);
  RUN_TEST(test_cooldown_is_capped_and_normal_values_pass);
  RUN_TEST(test_offline_threshold_zero_uses_default);
  RUN_TEST(test_offline_threshold_is_capped);
  RUN_TEST(test_burst_count_range);
  RUN_TEST(test_snapshot_dimension_zero_stays_unset);
  RUN_TEST(test_poll_interval_range);
  return UNITY_END();
}
