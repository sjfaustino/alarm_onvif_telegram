#include <unity.h>
#include <Arduino.h>
#include "camera_parse.h"

void setUp(void) {}
void tearDown(void) {}

// ---- parseProfiles ----

void test_parseProfiles_finds_token_and_name(void) {
  String xml = "<trt:GetProfilesResponse><trt:Profiles token=\"P1\">"
               "<tt:Name>MainStream</tt:Name></trt:Profiles></trt:GetProfilesResponse>";
  auto profiles = parseProfiles(xml);
  TEST_ASSERT_EQUAL_INT(1, (int)profiles.size());
  TEST_ASSERT_EQUAL_STRING("P1", profiles[0].token.c_str());
  TEST_ASSERT_EQUAL_STRING("MainStream", profiles[0].name.c_str());
}

void test_parseProfiles_finds_multiple_profiles_in_order(void) {
  String xml = "<a><trt:Profiles token=\"P1\"><tt:Name>Main</tt:Name></trt:Profiles>"
               "<trt:Profiles token=\"P2\"><tt:Name>Sub</tt:Name></trt:Profiles></a>";
  auto profiles = parseProfiles(xml);
  TEST_ASSERT_EQUAL_INT(2, (int)profiles.size());
  TEST_ASSERT_EQUAL_STRING("P1", profiles[0].token.c_str());
  TEST_ASSERT_EQUAL_STRING("P2", profiles[1].token.c_str());
}

void test_parseProfiles_tolerates_single_quoted_token(void) {
  String xml = "<trt:Profiles token='P1'><tt:Name>Main</tt:Name></trt:Profiles>";
  auto profiles = parseProfiles(xml);
  TEST_ASSERT_EQUAL_INT(1, (int)profiles.size());
  TEST_ASSERT_EQUAL_STRING("P1", profiles[0].token.c_str());
}

// A nested VideoEncoderConfiguration/VideoSourceConfiguration element also
// carries a "token=" attribute - only a real "...Profiles " opening tag
// should be picked up, not any bare "token=" in the document.
void test_parseProfiles_ignores_token_on_nested_configuration_elements(void) {
  String xml = "<trt:Profiles token=\"P1\">"
               "<tt:VideoEncoderConfiguration token=\"VEC1\"/>"
               "<tt:Name>Main</tt:Name></trt:Profiles>";
  auto profiles = parseProfiles(xml);
  TEST_ASSERT_EQUAL_INT(1, (int)profiles.size());
  TEST_ASSERT_EQUAL_STRING("P1", profiles[0].token.c_str());
}

void test_parseProfiles_missing_name_leaves_it_empty(void) {
  String xml = "<trt:Profiles token=\"P1\"></trt:Profiles>";
  auto profiles = parseProfiles(xml);
  TEST_ASSERT_EQUAL_INT(1, (int)profiles.size());
  TEST_ASSERT_EQUAL_STRING("", profiles[0].name.c_str());
}

void test_parseProfiles_no_profiles_returns_empty(void) {
  auto profiles = parseProfiles("<a><b>nothing here</b></a>");
  TEST_ASSERT_EQUAL_INT(0, (int)profiles.size());
}

// A profile with no token= attribute at all is skipped rather than
// returned with an empty token - camera.cpp treats an empty-token profile
// as unusable.
void test_parseProfiles_skips_profile_missing_token(void) {
  String xml = "<trt:Profiles fixed=\"true\"><tt:Name>Main</tt:Name></trt:Profiles>";
  auto profiles = parseProfiles(xml);
  TEST_ASSERT_EQUAL_INT(0, (int)profiles.size());
}

// ---- extractEventStateValue ----

void test_extractEventStateValue_finds_State_value(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>tns1:VideoSource/MotionAlarm</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  TEST_ASSERT_EQUAL_STRING("true", extractEventStateValue(xml, "MotionAlarm").c_str());
}

void test_extractEventStateValue_recognizes_IsMotion_name(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>CellMotionDetector</tt:Topic>"
               "<tt:SimpleItem Name=\"IsMotion\" Value=\"true\"/></wsnt:NotificationMessage>";
  TEST_ASSERT_EQUAL_STRING("true", extractEventStateValue(xml, "CellMotionDetector").c_str());
}

// Two NotificationMessage blocks for different topics - the search must
// stay scoped to the block containing the requested topic, not return
// whichever State happens to appear first in the whole document.
void test_extractEventStateValue_scoped_to_correct_notification_block(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>SignalLoss</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"false\"/></wsnt:NotificationMessage>"
               "<wsnt:NotificationMessage><tt:Topic>MotionAlarm</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  TEST_ASSERT_EQUAL_STRING("true", extractEventStateValue(xml, "MotionAlarm").c_str());
}

void test_extractEventStateValue_missing_topic_returns_empty(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>SignalLoss</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  TEST_ASSERT_EQUAL_STRING("", extractEventStateValue(xml, "MotionAlarm").c_str());
}

// ---- classifyCameraEvent ----

void test_classifyCameraEvent_detects_motion_alarm(void) {
  String xml = "<a>MotionAlarm<tt:SimpleItem Name=\"State\" Value=\"true\"/></a>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_TRUE(ev.motionAlarm);
  TEST_ASSERT_TRUE(ev.anyTrue);
  TEST_ASSERT_FALSE(ev.cellMotion);
  TEST_ASSERT_FALSE(ev.signalLoss);
  TEST_ASSERT_FALSE(ev.tamper);
}

// A batch that mentions a topic but reports it going back to false is not
// "any true" - triggerMotionAlert must not fire on this.
void test_classifyCameraEvent_value_false_is_not_anyTrue(void) {
  String xml = "<a>MotionAlarm<tt:SimpleItem Name=\"State\" Value=\"false\"/></a>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_TRUE(ev.motionAlarm);
  TEST_ASSERT_FALSE(ev.anyTrue);
}

void test_classifyCameraEvent_no_recognized_topic(void) {
  auto ev = classifyCameraEvent("<a>SomeOtherTopic Value=\"true\"</a>");
  TEST_ASSERT_FALSE(ev.motionAlarm);
  TEST_ASSERT_FALSE(ev.cellMotion);
  TEST_ASSERT_FALSE(ev.signalLoss);
  TEST_ASSERT_FALSE(ev.tamper);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_parseProfiles_finds_token_and_name);
  RUN_TEST(test_parseProfiles_finds_multiple_profiles_in_order);
  RUN_TEST(test_parseProfiles_tolerates_single_quoted_token);
  RUN_TEST(test_parseProfiles_ignores_token_on_nested_configuration_elements);
  RUN_TEST(test_parseProfiles_missing_name_leaves_it_empty);
  RUN_TEST(test_parseProfiles_no_profiles_returns_empty);
  RUN_TEST(test_parseProfiles_skips_profile_missing_token);
  RUN_TEST(test_extractEventStateValue_finds_State_value);
  RUN_TEST(test_extractEventStateValue_recognizes_IsMotion_name);
  RUN_TEST(test_extractEventStateValue_scoped_to_correct_notification_block);
  RUN_TEST(test_extractEventStateValue_missing_topic_returns_empty);
  RUN_TEST(test_classifyCameraEvent_detects_motion_alarm);
  RUN_TEST(test_classifyCameraEvent_value_false_is_not_anyTrue);
  RUN_TEST(test_classifyCameraEvent_no_recognized_topic);
  return UNITY_END();
}
