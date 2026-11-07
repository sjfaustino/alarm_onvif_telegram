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

// A namespaced element whose *closing* tag doesn't repeat the prefix its
// opening tag used (some stacks are inconsistent about this) must still
// resolve to the right end, not fall through to accepting whatever "</"
// happens to come first.
void test_findElementByLocalName_closing_tag_without_matching_prefix(void) {
  String r = findElementByLocalName("<trt:Uri>http://cam/x.jpg</Uri>", "Uri");
  TEST_ASSERT_EQUAL_STRING("http://cam/x.jpg", r.c_str());
}

// A nested child element between the opening and true closing tag must
// not be mistaken for the element's own end - this is exactly the gap
// fixed by comparing each candidate closing tag's own local name instead
// of accepting the first "</" found.
void test_findElementByLocalName_skips_nested_child_closing_tag(void) {
  String xml = "<trt:Profiles><tt:Name>Sub</tt:Name>ignored-if-this-were-returned</trt:Profiles>";
  String r = findElementByLocalName(xml, "Profiles");
  TEST_ASSERT_EQUAL_STRING("<tt:Name>Sub</tt:Name>ignored-if-this-were-returned", r.c_str());
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

// Not every ONVIF stack is consistent about double-quoting attributes -
// this is a real gap that used to exist (findAttributeValue's pattern was
// hardcoded to attributeName + "=\"" only).
void test_findAttributeValue_tolerates_single_quotes(void) {
  String r = findAttributeValue("<Profiles token='Profile_1' fixed='true'>", "Profiles", "token");
  TEST_ASSERT_EQUAL_STRING("Profile_1", r.c_str());
}

void test_findAttributeValue_does_not_match_substring_inside_a_longer_element_name(void) {
  // "Profiles" is a substring of "VideoProfiles" - a plain xml.indexOf()
  // search for the element name (the old implementation) would have
  // wandered into this element's attributes instead of failing to find a
  // real <Profiles> element.
  String r = findAttributeValue("<VideoProfiles token=\"wrong\">", "Profiles", "token");
  TEST_ASSERT_EQUAL_STRING("", r.c_str());
}

// ---- findAttributeInTag ----

void test_findAttributeInTag_tolerates_single_and_double_quotes(void) {
  TEST_ASSERT_EQUAL_STRING("P1", findAttributeInTag("Profiles token=\"P1\"", "token").c_str());
  TEST_ASSERT_EQUAL_STRING("P1", findAttributeInTag("Profiles token='P1'", "token").c_str());
}

// "id" must not match inside the longer attribute name "profileid" - only
// a real word-boundary-anchored "id=" counts.
void test_findAttributeInTag_does_not_match_inside_a_longer_attribute_name(void) {
  String r = findAttributeInTag("Profiles profileid=\"wrong\" id=\"right\"", "id");
  TEST_ASSERT_EQUAL_STRING("right", r.c_str());
}

void test_findAttributeInTag_missing_attribute_returns_empty(void) {
  TEST_ASSERT_EQUAL_STRING("", findAttributeInTag("Profiles fixed=\"true\"", "token").c_str());
}

// ---- responseHasFault ----

void test_responseHasFault_detects_soap_fault(void) {
  TEST_ASSERT_TRUE(responseHasFault("<s:Envelope><s:Body><s:Fault>...</s:Fault></s:Body></s:Envelope>"));
}

void test_responseHasFault_false_on_normal_response(void) {
  TEST_ASSERT_FALSE(responseHasFault("<s:Envelope><s:Body><tds:GetCapabilitiesResponse/></s:Body></s:Envelope>"));
}

// A SOAP server declaring the envelope as the default (unprefixed)
// namespace emits a bare <Fault>, not "<s:Fault"/":Fault" - the bug this
// exists to catch: cameraPullMessages (camera.cpp) never resets
// subscriptionActive on a response that's neither PullMessagesResponse
// nor a detected fault, so an undetected bare Fault for an expired
// subscription would wedge that camera's polling loop forever.
void test_responseHasFault_detects_unprefixed_default_namespace_fault(void) {
  TEST_ASSERT_TRUE(responseHasFault("<Envelope><Body><Fault><faultcode>x</faultcode></Fault></Body></Envelope>"));
  TEST_ASSERT_TRUE(responseHasFault("<Envelope><Body><Fault/></Body></Envelope>"));
}

// Must not false-positive on an unrelated element that merely starts with
// "Fault" - only an actual "Fault" tag (boundary right after it) counts.
void test_responseHasFault_no_false_positive_on_similarly_named_element(void) {
  TEST_ASSERT_FALSE(responseHasFault("<Envelope><Body><FaultInfo>not a fault</FaultInfo></Body></Envelope>"));
}

void test_soapFaultSummary_soap12_prefers_subcode_and_handles_attributes(void) {
  const char* xml = "<SOAP-ENV:Envelope><SOAP-ENV:Header><wsa5:Action SOAP-ENV:mustUnderstand=\"1\">"
    "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesResponse</wsa5:Action>"
    "</SOAP-ENV:Header><SOAP-ENV:Body><SOAP-ENV:Fault><SOAP-ENV:Code><SOAP-ENV:Value>SOAP-ENV:Sender"
    "</SOAP-ENV:Value><SOAP-ENV:Subcode><SOAP-ENV:Value>ter:NotAuthorized</SOAP-ENV:Value></SOAP-ENV:Subcode>"
    "</SOAP-ENV:Code><SOAP-ENV:Reason><SOAP-ENV:Text xml:lang=\"en\">Authority failure</SOAP-ENV:Text>"
    "</SOAP-ENV:Reason></SOAP-ENV:Fault></SOAP-ENV:Body></SOAP-ENV:Envelope>";
  TEST_ASSERT_EQUAL_STRING("ter:NotAuthorized: Authority failure", soapFaultSummary(xml).c_str());
}

void test_soapFaultSummary_soap12_without_subcode(void) {
  TEST_ASSERT_EQUAL_STRING("env:Sender: Bad",
                           soapFaultSummary("<env:Fault><env:Code><env:Value>env:Sender</env:Value></env:Code>"
                                            "<env:Reason><env:Text>Bad</env:Text></env:Reason></env:Fault>")
                               .c_str());
}

void test_soapFaultSummary_soap11(void) {
  TEST_ASSERT_EQUAL_STRING("s:Client: Denied",
                           soapFaultSummary("<s:Fault><faultcode>s:Client</faultcode>"
                                            "<faultstring>Denied</faultstring></s:Fault>")
                               .c_str());
}

void test_soapFaultSummary_ignores_matching_names_before_the_fault(void) {
  TEST_ASSERT_EQUAL_STRING("c: r", soapFaultSummary("<a:Value>header</a:Value><a:Text>header</a:Text>"
                                                    "<a:Fault><faultcode>c</faultcode><faultstring>r</faultstring></a:Fault>")
                                       .c_str());
}

void test_soapFaultSummary_unparseable_falls_back_to_a_short_prefix(void) {
  String longBody;
  for (int i = 0; i < 500; i++) longBody += "x";
  TEST_ASSERT_EQUAL(200, (int)soapFaultSummary(longBody).length());
  TEST_ASSERT_EQUAL_STRING("<a:Fault/>", soapFaultSummary("<a:Fault/>").c_str());
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
  RUN_TEST(test_findElementByLocalName_closing_tag_without_matching_prefix);
  RUN_TEST(test_findElementByLocalName_skips_nested_child_closing_tag);
  RUN_TEST(test_findAttributeValue_finds_quoted_attribute);
  RUN_TEST(test_findAttributeValue_missing_attribute_returns_empty);
  RUN_TEST(test_findAttributeValue_missing_element_returns_empty);
  RUN_TEST(test_findAttributeValue_tolerates_single_quotes);
  RUN_TEST(test_findAttributeValue_does_not_match_substring_inside_a_longer_element_name);
  RUN_TEST(test_findAttributeInTag_tolerates_single_and_double_quotes);
  RUN_TEST(test_findAttributeInTag_does_not_match_inside_a_longer_attribute_name);
  RUN_TEST(test_findAttributeInTag_missing_attribute_returns_empty);
  RUN_TEST(test_responseHasFault_detects_soap_fault);
  RUN_TEST(test_responseHasFault_false_on_normal_response);
  RUN_TEST(test_responseHasFault_detects_unprefixed_default_namespace_fault);
  RUN_TEST(test_responseHasFault_no_false_positive_on_similarly_named_element);
  RUN_TEST(test_soapFaultSummary_soap12_prefers_subcode_and_handles_attributes);
  RUN_TEST(test_soapFaultSummary_soap12_without_subcode);
  RUN_TEST(test_soapFaultSummary_soap11);
  RUN_TEST(test_soapFaultSummary_ignores_matching_names_before_the_fault);
  RUN_TEST(test_soapFaultSummary_unparseable_falls_back_to_a_short_prefix);
  return UNITY_END();
}
