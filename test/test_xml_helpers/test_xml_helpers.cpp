#include <unity.h>
#include <Arduino.h>
#include "xml_helpers.h"

void setUp(void) {}
void tearDown(void) {}

// ---- xmlEscape ----

void test_xmlEscape_escapes_all_five_entities(void) {
  TEST_ASSERT_EQUAL_STRING("&amp;&lt;&gt;&quot;&apos;", xmlEscape("&<>\"'").c_str());
}

void test_xmlEscape_leaves_plain_text_untouched(void) {
  TEST_ASSERT_EQUAL_STRING("D01-FrontDoor", xmlEscape("D01-FrontDoor").c_str());
}

void test_xmlEscape_empty_string(void) {
  TEST_ASSERT_EQUAL_STRING("", xmlEscape("").c_str());
}

// ---- findElementByLocalName ----

void test_findElementByLocalName_unprefixed_tag(void) {
  String r = findElementByLocalName("<root><Uri>http://cam/snap.jpg</Uri></root>", "Uri");
  TEST_ASSERT_EQUAL_STRING("http://cam/snap.jpg", r.c_str());
}

void test_findElementByLocalName_namespaced_tag_falls_back_to_suffix_match(void) {
  // No bare "<Uri>" exists here, only a namespaced one - exercises the
  // second search branch (":Uri>" suffix match).
  String r = findElementByLocalName("<trt:GetSnapshotUriResponse><trt:Uri>http://cam/x.jpg</trt:Uri>"
                                     "</trt:GetSnapshotUriResponse>",
                                     "Uri");
  TEST_ASSERT_EQUAL_STRING("http://cam/x.jpg", r.c_str());
}

void test_findElementByLocalName_trims_whitespace(void) {
  String r = findElementByLocalName("<a><Uri>\n  http://cam/x.jpg  \n</Uri></a>", "Uri");
  TEST_ASSERT_EQUAL_STRING("http://cam/x.jpg", r.c_str());
}

void test_findElementByLocalName_missing_element_returns_empty(void) {
  String r = findElementByLocalName("<root><Other>x</Other></root>", "Uri");
  TEST_ASSERT_EQUAL_STRING("", r.c_str());
}

void test_findElementByLocalName_respects_fromPosition(void) {
  // Two <State> blocks - the real use case (camera.cpp's printEventState)
  // scopes the search to one NotificationMessage block by passing
  // fromPosition, so the first (wrong) block must not be found.
  String xml = "<a>ignored<State>false</State></a><b>real<State>true</State></b>";
  int scopeStart = xml.indexOf("<b>");
  String r = findElementByLocalName(xml, "State", scopeStart);
  TEST_ASSERT_EQUAL_STRING("true", r.c_str());
}

// ---- findAttributeValue ----

void test_findAttributeValue_finds_quoted_attribute(void) {
  String r = findAttributeValue("<Profiles token=\"Profile_1\" fixed=\"true\">", "Profiles", "token");
  TEST_ASSERT_EQUAL_STRING("Profile_1", r.c_str());
}

void test_findAttributeValue_missing_attribute_returns_empty(void) {
  String r = findAttributeValue("<Profiles fixed=\"true\">", "Profiles", "token");
  TEST_ASSERT_EQUAL_STRING("", r.c_str());
}

void test_findAttributeValue_missing_element_returns_empty(void) {
  String r = findAttributeValue("<Other token=\"x\">", "Profiles", "token");
  TEST_ASSERT_EQUAL_STRING("", r.c_str());
}

// ---- responseHasFault ----

void test_responseHasFault_detects_soap_fault(void) {
  TEST_ASSERT_TRUE(responseHasFault("<s:Envelope><s:Body><s:Fault>...</s:Fault></s:Body></s:Envelope>"));
}

void test_responseHasFault_false_on_normal_response(void) {
  TEST_ASSERT_FALSE(responseHasFault("<s:Envelope><s:Body><tds:GetCapabilitiesResponse/></s:Body></s:Envelope>"));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_xmlEscape_escapes_all_five_entities);
  RUN_TEST(test_xmlEscape_leaves_plain_text_untouched);
  RUN_TEST(test_xmlEscape_empty_string);
  RUN_TEST(test_findElementByLocalName_unprefixed_tag);
  RUN_TEST(test_findElementByLocalName_namespaced_tag_falls_back_to_suffix_match);
  RUN_TEST(test_findElementByLocalName_trims_whitespace);
  RUN_TEST(test_findElementByLocalName_missing_element_returns_empty);
  RUN_TEST(test_findElementByLocalName_respects_fromPosition);
  RUN_TEST(test_findAttributeValue_finds_quoted_attribute);
  RUN_TEST(test_findAttributeValue_missing_attribute_returns_empty);
  RUN_TEST(test_findAttributeValue_missing_element_returns_empty);
  RUN_TEST(test_responseHasFault_detects_soap_fault);
  RUN_TEST(test_responseHasFault_false_on_normal_response);
  return UNITY_END();
}
