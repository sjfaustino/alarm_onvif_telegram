#include <unity.h>
#include <Arduino.h>
#include <climits> // ULONG_MAX
#include "net_watchdog_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---- isReservedOrUnsafePin ----

void test_isReservedOrUnsafePin_rejects_sd_pins(void) {
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(10)); // SD_CS_PIN
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(12)); // SD_SCK_PIN
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(13)); // SD_MISO_PIN
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(11)); // SD_MOSI_PIN
}

void test_isReservedOrUnsafePin_rejects_rtc_pins(void) {
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(8)); // RTC_SDA_PIN
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(9)); // RTC_SCL_PIN
}

void test_isReservedOrUnsafePin_rejects_strapping_pins(void) {
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(0));
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(3));
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(45));
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(46));
}

// The specific, highest-consequence case this function exists to prevent:
// any pin in the flash/octal-PSRAM range must never be accepted, since
// driving one is a hard hang on boot for an unattended, remotely-deployed
// board - not merely "the wrong pin got toggled."
void test_isReservedOrUnsafePin_rejects_full_flash_psram_range(void) {
  for (int pin = 26; pin <= 37; pin++) {
    TEST_ASSERT_TRUE(isReservedOrUnsafePin(pin));
  }
}

void test_isReservedOrUnsafePin_rejects_usb_and_uart0_pins(void) {
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(19));
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(20));
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(43));
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(44));
}

void test_isReservedOrUnsafePin_rejects_out_of_range_values(void) {
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(-1));
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(49));
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(9999)); // a plausible garbage value from a corrupted NVS record
}

// The default this project actually prefills (NET_WATCHDOG_PIN_DEFAULT,
// config.h) and a couple of other ordinary, commonly-free ESP32-S3 GPIOs
// must be accepted - the whole feature is unusable if this function is
// overzealous.
void test_isReservedOrUnsafePin_accepts_ordinary_pins(void) {
  TEST_ASSERT_FALSE(isReservedOrUnsafePin(4));
  TEST_ASSERT_FALSE(isReservedOrUnsafePin(5));
  TEST_ASSERT_FALSE(isReservedOrUnsafePin(6));
  TEST_ASSERT_FALSE(isReservedOrUnsafePin(7));
  TEST_ASSERT_FALSE(isReservedOrUnsafePin(15));
  TEST_ASSERT_FALSE(isReservedOrUnsafePin(48));
}

// The boundaries right next to the flash/PSRAM range must still be
// individually correct - a fencepost error here would either leave a real
// hang-risk pin accepted or reject an otherwise-fine neighboring pin.
void test_isReservedOrUnsafePin_flash_psram_range_boundaries(void) {
  TEST_ASSERT_FALSE(isReservedOrUnsafePin(25)); // just below the range
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(26));  // first reserved
  TEST_ASSERT_TRUE(isReservedOrUnsafePin(37));  // last reserved
  TEST_ASSERT_FALSE(isReservedOrUnsafePin(38)); // just above the range
}

// ---- outageThresholdReached ----

void test_outageThresholdReached_false_below_threshold(void) {
  TEST_ASSERT_FALSE(outageThresholdReached(1000, 5000, 10000)); // only 4s elapsed of a 10s threshold
}

void test_outageThresholdReached_true_exactly_at_threshold(void) {
  TEST_ASSERT_TRUE(outageThresholdReached(1000, 11000, 10000)); // exactly 10s elapsed
}

void test_outageThresholdReached_true_above_threshold(void) {
  TEST_ASSERT_TRUE(outageThresholdReached(1000, 20000, 10000));
}

// millis() wraparound: nowMs has rolled over past firstFailureMs - the
// unsigned subtraction must still produce the correct real elapsed
// duration, not treat this as "no time has passed" or crash. Expressed
// relative to ULONG_MAX rather than a hardcoded 32-bit literal
// (0xFFFFFFF0) on purpose - `unsigned long` is 32-bit on Windows/ESP32
// but 64-bit on Linux (LP64), so a fixed 32-bit-max-adjacent constant
// would only actually sit "just before wraparound" on some platforms and
// silently stop testing anything meaningful (or fail outright) on
// others - exactly what broke this test the first time, passing locally
// on Windows but failing on Linux CI.
void test_outageThresholdReached_survives_millis_wraparound(void) {
  unsigned long firstFailureMs = ULONG_MAX - 15UL; // 16 values before wraparound, whatever width unsigned long has here
  unsigned long nowMs = 20UL;                       // 20 past wraparound
  // Real elapsed time: 16 (to reach the wrap point, inclusive) + 20 (past it) = 36.
  TEST_ASSERT_TRUE(outageThresholdReached(firstFailureMs, nowMs, 30));
  TEST_ASSERT_FALSE(outageThresholdReached(firstFailureMs, nowMs, 40));
}

// ---- watchdogPinsConflict ----

void test_watchdogPinsConflict_true_when_both_enabled_same_pin(void) {
  TEST_ASSERT_TRUE(watchdogPinsConflict(true, 5, true, 5));
}

void test_watchdogPinsConflict_false_when_either_disabled(void) {
  TEST_ASSERT_FALSE(watchdogPinsConflict(false, 5, true, 5));
  TEST_ASSERT_FALSE(watchdogPinsConflict(true, 5, false, 5));
  TEST_ASSERT_FALSE(watchdogPinsConflict(false, 5, false, 5));
}

void test_watchdogPinsConflict_false_when_pins_differ(void) {
  TEST_ASSERT_FALSE(watchdogPinsConflict(true, 4, true, 5));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_isReservedOrUnsafePin_rejects_sd_pins);
  RUN_TEST(test_isReservedOrUnsafePin_rejects_rtc_pins);
  RUN_TEST(test_isReservedOrUnsafePin_rejects_strapping_pins);
  RUN_TEST(test_isReservedOrUnsafePin_rejects_full_flash_psram_range);
  RUN_TEST(test_isReservedOrUnsafePin_rejects_usb_and_uart0_pins);
  RUN_TEST(test_isReservedOrUnsafePin_rejects_out_of_range_values);
  RUN_TEST(test_isReservedOrUnsafePin_accepts_ordinary_pins);
  RUN_TEST(test_isReservedOrUnsafePin_flash_psram_range_boundaries);
  RUN_TEST(test_outageThresholdReached_false_below_threshold);
  RUN_TEST(test_outageThresholdReached_true_exactly_at_threshold);
  RUN_TEST(test_outageThresholdReached_true_above_threshold);
  RUN_TEST(test_outageThresholdReached_survives_millis_wraparound);
  RUN_TEST(test_watchdogPinsConflict_true_when_both_enabled_same_pin);
  RUN_TEST(test_watchdogPinsConflict_false_when_either_disabled);
  RUN_TEST(test_watchdogPinsConflict_false_when_pins_differ);
  return UNITY_END();
}
