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

// The bug this exists to catch: PullMessages can return several
// NotificationMessage blocks for the SAME topic in one batch (MessageLimit
// is 20 - camera.cpp) - a stale/trailing "false" followed later in the
// same batch by a genuine new "true". The old implementation only ever
// looked at the FIRST block mentioning the topic and stopped, so this real
// motion event would have been silently invisible.
void test_extractEventStateValue_finds_true_in_a_later_block_for_same_topic(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>MotionAlarm</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"false\"/></wsnt:NotificationMessage>"
               "<wsnt:NotificationMessage><tt:Topic>MotionAlarm</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  TEST_ASSERT_EQUAL_STRING("true", extractEventStateValue(xml, "MotionAlarm").c_str());
}

// Same shape, but every block for the topic agrees "false" - must not
// spuriously report true just because more than one block was scanned.
void test_extractEventStateValue_all_blocks_false_stays_false(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>MotionAlarm</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"false\"/></wsnt:NotificationMessage>"
               "<wsnt:NotificationMessage><tt:Topic>MotionAlarm</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"false\"/></wsnt:NotificationMessage>";
  TEST_ASSERT_EQUAL_STRING("false", extractEventStateValue(xml, "MotionAlarm").c_str());
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

// The specific vendor topic seen in the field (tns1:RuleEngine/
// MyRuleDetector/PeopleDetect) - matched by the "PeopleDetect" substring,
// same convention as MotionAlarm/CellMotionDetector/TamperDetector/SignalLoss.
void test_classifyCameraEvent_detects_people_detect(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>tns1:RuleEngine/MyRuleDetector/PeopleDetect"
               "</tt:Topic><tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_TRUE(ev.peopleDetect);
  TEST_ASSERT_TRUE(ev.anyTrue);
  TEST_ASSERT_FALSE(ev.motionAlarm);
  TEST_ASSERT_FALSE(ev.cellMotion);
}

// Same vendor's RuleEngine, its vehicle-detection cell - seen from the same
// camera in the field alongside PeopleDetect.
void test_classifyCameraEvent_detects_vehicle_detect(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>tns1:RuleEngine/MyRuleDetector/VehicleDetect"
               "</tt:Topic><tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_TRUE(ev.vehicleDetect);
  TEST_ASSERT_TRUE(ev.anyTrue);
  TEST_ASSERT_FALSE(ev.peopleDetect);
}

void test_classifyCameraEvent_no_recognized_topic(void) {
  auto ev = classifyCameraEvent("<a>SomeOtherTopic Value=\"true\"</a>");
  TEST_ASSERT_FALSE(ev.motionAlarm);
  TEST_ASSERT_FALSE(ev.cellMotion);
  TEST_ASSERT_FALSE(ev.peopleDetect);
  TEST_ASSERT_FALSE(ev.vehicleDetect);
  TEST_ASSERT_FALSE(ev.signalLoss);
  TEST_ASSERT_FALSE(ev.tamper);
}

// ---- motionEventFired ----

// A batch where the motion topic itself reports true - the baseline case.
void test_motionEventFired_true_for_motion_alarm_reporting_true(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>MotionAlarm</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_TRUE(motionEventFired(xml, ev));
}

// The bug this exists to catch: CellMotionDetector's own state in this
// batch is false (motion just ended), but an unrelated SignalLoss topic in
// the same PullMessages response reports Value="true", which used to make
// ev.anyTrue true and fire a motion alert anyway. motionEventFired must
// check CellMotionDetector's own scoped state, not the body-wide flag.
void test_motionEventFired_false_when_only_an_unrelated_topic_is_true(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>CellMotionDetector</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"false\"/></wsnt:NotificationMessage>"
               "<wsnt:NotificationMessage><tt:Topic>SignalLoss</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_TRUE(ev.anyTrue);   // sanity: the old, buggy signal is still true here
  TEST_ASSERT_TRUE(ev.cellMotion);
  TEST_ASSERT_FALSE(motionEventFired(xml, ev)); // but the fix must not fire on it
}

// Same shape, but the motion topic itself is the one that's true - must
// still fire even with an unrelated topic also present in the batch.
void test_motionEventFired_true_when_motion_topic_itself_is_true_alongside_another(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>TamperDetector</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"false\"/></wsnt:NotificationMessage>"
               "<wsnt:NotificationMessage><tt:Topic>MotionAlarm</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_TRUE(motionEventFired(xml, ev));
}

// PeopleDetect is a motion-relevant topic too, same as MotionAlarm/
// CellMotionDetector - the specific case reported in the field
// (tns1:RuleEngine/MyRuleDetector/PeopleDetect), previously only ever
// logged as "unrecognized" and never fired an alert.
void test_motionEventFired_true_for_people_detect_reporting_true(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>tns1:RuleEngine/MyRuleDetector/PeopleDetect"
               "</tt:Topic><tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_TRUE(motionEventFired(xml, ev));
}

void test_motionEventFired_true_for_vehicle_detect_reporting_true(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>tns1:RuleEngine/MyRuleDetector/VehicleDetect"
               "</tt:Topic><tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_TRUE(motionEventFired(xml, ev));
}

// No motion-relevant topic present at all (only Tamper) - never fires,
// regardless of anyTrue.
void test_motionEventFired_false_when_no_motion_topic_present(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>TamperDetector</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  auto ev = classifyCameraEvent(xml);
  TEST_ASSERT_FALSE(motionEventFired(xml, ev));
}

// ---- topicReportedTrue ----

void test_topicReportedTrue_true_when_state_is_true(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>TamperDetector</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  TEST_ASSERT_TRUE(topicReportedTrue(xml, "TamperDetector"));
}

void test_topicReportedTrue_false_when_state_is_false(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>SignalLoss</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"false\"/></wsnt:NotificationMessage>";
  TEST_ASSERT_FALSE(topicReportedTrue(xml, "SignalLoss"));
}

// The same cross-topic scenario motionEventFired guards against, but for
// the general-purpose primitive it (and camera.cpp's tamper/signal-loss
// checks) is built from.
void test_topicReportedTrue_ignores_unrelated_topics_true_value(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>TamperDetector</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"false\"/></wsnt:NotificationMessage>"
               "<wsnt:NotificationMessage><tt:Topic>SignalLoss</tt:Topic>"
               "<tt:SimpleItem Name=\"State\" Value=\"true\"/></wsnt:NotificationMessage>";
  TEST_ASSERT_FALSE(topicReportedTrue(xml, "TamperDetector"));
  TEST_ASSERT_TRUE(topicReportedTrue(xml, "SignalLoss"));
}

// ---- firstTopic ----

// Real ONVIF Topic elements almost always carry a Dialect attribute - the
// case findElementByLocalName (xml_helpers.h) isn't built for, and the
// reason firstTopic has its own bespoke parsing instead of reusing it.
void test_firstTopic_extracts_content_with_dialect_attribute(void) {
  String xml = "<wsnt:NotificationMessage><wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/"
               "topicExpression/ConcreteSet\">tns1:RuleEngine/MyRuleDetector/PeopleDetect"
               "</wsnt:Topic></wsnt:NotificationMessage>";
  TEST_ASSERT_EQUAL_STRING("tns1:RuleEngine/MyRuleDetector/PeopleDetect", firstTopic(xml).c_str());
}

void test_firstTopic_extracts_content_without_attributes(void) {
  String xml = "<wsnt:NotificationMessage><tt:Topic>tns1:VideoSource/MotionAlarm</tt:Topic>"
               "</wsnt:NotificationMessage>";
  TEST_ASSERT_EQUAL_STRING("tns1:VideoSource/MotionAlarm", firstTopic(xml).c_str());
}

void test_firstTopic_returns_empty_when_no_topic_element(void) {
  TEST_ASSERT_EQUAL_STRING("", firstTopic("<wsnt:NotificationMessage></wsnt:NotificationMessage>").c_str());
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
  RUN_TEST(test_extractEventStateValue_finds_true_in_a_later_block_for_same_topic);
  RUN_TEST(test_extractEventStateValue_all_blocks_false_stays_false);
  RUN_TEST(test_extractEventStateValue_missing_topic_returns_empty);
  RUN_TEST(test_classifyCameraEvent_detects_motion_alarm);
  RUN_TEST(test_classifyCameraEvent_value_false_is_not_anyTrue);
  RUN_TEST(test_classifyCameraEvent_detects_people_detect);
  RUN_TEST(test_classifyCameraEvent_detects_vehicle_detect);
  RUN_TEST(test_classifyCameraEvent_no_recognized_topic);
  RUN_TEST(test_motionEventFired_true_for_motion_alarm_reporting_true);
  RUN_TEST(test_motionEventFired_true_for_people_detect_reporting_true);
  RUN_TEST(test_motionEventFired_true_for_vehicle_detect_reporting_true);
  RUN_TEST(test_motionEventFired_false_when_only_an_unrelated_topic_is_true);
  RUN_TEST(test_motionEventFired_true_when_motion_topic_itself_is_true_alongside_another);
  RUN_TEST(test_motionEventFired_false_when_no_motion_topic_present);
  RUN_TEST(test_topicReportedTrue_true_when_state_is_true);
  RUN_TEST(test_topicReportedTrue_false_when_state_is_false);
  RUN_TEST(test_topicReportedTrue_ignores_unrelated_topics_true_value);
  RUN_TEST(test_firstTopic_extracts_content_with_dialect_attribute);
  RUN_TEST(test_firstTopic_extracts_content_without_attributes);
  RUN_TEST(test_firstTopic_returns_empty_when_no_topic_element);
  return UNITY_END();
}
