#include <unity.h>
#include "debug_token.h"

void setUp(void) {}
void tearDown(void) {}

void test_matching_token_is_valid(void) { TEST_ASSERT_TRUE(debugTokenValid("s3cret", "s3cret")); }
void test_wrong_token_is_rejected(void) { TEST_ASSERT_FALSE(debugTokenValid("s3cret", "s3creX")); }
void test_prefix_is_rejected(void) {
  TEST_ASSERT_FALSE(debugTokenValid("s3cret", "s3c"));
  TEST_ASSERT_FALSE(debugTokenValid("s3c", "s3cret"));
}
void test_empty_configured_token_disables_access(void) {
  TEST_ASSERT_FALSE(debugTokenValid("", ""));
  TEST_ASSERT_FALSE(debugTokenValid("", "anything"));
}
void test_missing_values_are_rejected(void) {
  TEST_ASSERT_FALSE(debugTokenValid(nullptr, "x"));
  TEST_ASSERT_FALSE(debugTokenValid("x", nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_matching_token_is_valid);
  RUN_TEST(test_wrong_token_is_rejected);
  RUN_TEST(test_prefix_is_rejected);
  RUN_TEST(test_empty_configured_token_disables_access);
  RUN_TEST(test_missing_values_are_rejected);
  return UNITY_END();
}
