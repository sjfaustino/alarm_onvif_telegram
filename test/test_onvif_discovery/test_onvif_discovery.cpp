#include <unity.h>
#include <Arduino.h>
#include "onvif_discovery.h"

void setUp(void) {}
void tearDown(void) {}

// ---- buildProbeMessage ----

void test_buildProbeMessage_includes_messageId(void) {
  String xml = buildProbeMessage("urn:uuid:1234");
  TEST_ASSERT_TRUE(xml.indexOf("<w:MessageID>urn:uuid:1234</w:MessageID>") >= 0);
}

void test_buildProbeMessage_has_no_Types_filter(void) {
  // Deliberate - see the header's comment on buildProbeMessage for why an
  // unfiltered Probe is used instead of a NetworkVideoTransmitter Types
  // restriction.
  String xml = buildProbeMessage("urn:uuid:1234");
  TEST_ASSERT_TRUE(xml.indexOf("<d:Probe/>") >= 0);
  TEST_ASSERT_TRUE(xml.indexOf("Types") < 0);
}

void test_buildProbeMessage_addresses_the_discovery_action(void) {
  String xml = buildProbeMessage("urn:uuid:1234");
  TEST_ASSERT_TRUE(xml.indexOf("http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe") >= 0);
}

// ---- parseProbeMatch ----

void test_parseProbeMatch_extracts_single_xaddr(void) {
  String xml = "<e:Envelope><e:Body><d:ProbeMatches><d:ProbeMatch>"
               "<d:Scopes>onvif://www.onvif.org/type/video_encoder</d:Scopes>"
               "<d:XAddrs>http://192.168.1.50/onvif/device_service</d:XAddrs>"
               "</d:ProbeMatch></d:ProbeMatches></e:Body></e:Envelope>";
  DiscoveredCamera dc;
  TEST_ASSERT_TRUE(parseProbeMatch(xml, dc));
  TEST_ASSERT_EQUAL_STRING("http://192.168.1.50/onvif/device_service", dc.xaddr.c_str());
  TEST_ASSERT_EQUAL_STRING("", dc.nameHint.c_str());
}

void test_parseProbeMatch_prefers_http_over_https_in_multi_xaddr(void) {
  String xml = "<d:ProbeMatch><d:XAddrs>https://192.168.1.50/onvif/device_service "
               "http://192.168.1.50/onvif/device_service</d:XAddrs></d:ProbeMatch>";
  DiscoveredCamera dc;
  TEST_ASSERT_TRUE(parseProbeMatch(xml, dc));
  TEST_ASSERT_EQUAL_STRING("http://192.168.1.50/onvif/device_service", dc.xaddr.c_str());
}

void test_parseProbeMatch_falls_back_to_first_token_when_none_are_http(void) {
  String xml = "<d:ProbeMatch><d:XAddrs>https://192.168.1.50/onvif/device_service</d:XAddrs></d:ProbeMatch>";
  DiscoveredCamera dc;
  TEST_ASSERT_TRUE(parseProbeMatch(xml, dc));
  TEST_ASSERT_EQUAL_STRING("https://192.168.1.50/onvif/device_service", dc.xaddr.c_str());
}

void test_parseProbeMatch_extracts_name_hint_from_scopes(void) {
  String xml = "<d:ProbeMatch>"
               "<d:Scopes>onvif://www.onvif.org/type/video_encoder "
               "onvif://www.onvif.org/name/Front%20Door "
               "onvif://www.onvif.org/hardware/IPC-123</d:Scopes>"
               "<d:XAddrs>http://192.168.1.50/onvif/device_service</d:XAddrs>"
               "</d:ProbeMatch>";
  DiscoveredCamera dc;
  TEST_ASSERT_TRUE(parseProbeMatch(xml, dc));
  TEST_ASSERT_EQUAL_STRING("Front Door", dc.nameHint.c_str());
}

void test_parseProbeMatch_falls_back_to_hardware_hint_when_no_name_scope(void) {
  String xml = "<d:ProbeMatch>"
               "<d:Scopes>onvif://www.onvif.org/hardware/IPC-123</d:Scopes>"
               "<d:XAddrs>http://192.168.1.50/onvif/device_service</d:XAddrs>"
               "</d:ProbeMatch>";
  DiscoveredCamera dc;
  TEST_ASSERT_TRUE(parseProbeMatch(xml, dc));
  TEST_ASSERT_EQUAL_STRING("IPC-123", dc.nameHint.c_str());
}

void test_parseProbeMatch_returns_false_without_ProbeMatch(void) {
  // Noise from an unrelated device answering the unfiltered Probe (a NAS,
  // a printer) - no ProbeMatch element at all.
  String xml = "<e:Envelope><e:Body><SomethingElse/></e:Body></e:Envelope>";
  DiscoveredCamera dc;
  TEST_ASSERT_FALSE(parseProbeMatch(xml, dc));
}

void test_parseProbeMatch_returns_false_without_xaddrs(void) {
  String xml = "<d:ProbeMatch><d:Scopes>onvif://www.onvif.org/name/Foo</d:Scopes></d:ProbeMatch>";
  DiscoveredCamera dc;
  TEST_ASSERT_FALSE(parseProbeMatch(xml, dc));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_buildProbeMessage_includes_messageId);
  RUN_TEST(test_buildProbeMessage_has_no_Types_filter);
  RUN_TEST(test_buildProbeMessage_addresses_the_discovery_action);
  RUN_TEST(test_parseProbeMatch_extracts_single_xaddr);
  RUN_TEST(test_parseProbeMatch_prefers_http_over_https_in_multi_xaddr);
  RUN_TEST(test_parseProbeMatch_falls_back_to_first_token_when_none_are_http);
  RUN_TEST(test_parseProbeMatch_extracts_name_hint_from_scopes);
  RUN_TEST(test_parseProbeMatch_falls_back_to_hardware_hint_when_no_name_scope);
  RUN_TEST(test_parseProbeMatch_returns_false_without_ProbeMatch);
  RUN_TEST(test_parseProbeMatch_returns_false_without_xaddrs);
  return UNITY_END();
}
