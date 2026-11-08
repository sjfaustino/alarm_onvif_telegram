#include <unity.h>
#include "camera_hardware.h"

void setUp(void) {}
void tearDown(void) {}

// Shapes from real cameras (a Vatilon H80, a Reolink RLC-520A, a Tapo TC65,
// a Vstarcam), with serial numbers and MAC addresses changed.

static const char* kVatilonDeviceInfo =
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"><SOAP-ENV:Body>\n"
    "<tds:GetDeviceInformationResponse>\n"
    "<tds:Manufacturer>Vatilon</tds:Manufacturer>\n"
    "<tds:Model>H80</tds:Model>\n"
    "<tds:FirmwareVersion>V1.16.25-20250614</tds:FirmwareVersion>\n"
    "<tds:SerialNumber>0011223344556677</tds:SerialNumber>\n"
    "<tds:HardwareId>H80</tds:HardwareId>\n"
    "</tds:GetDeviceInformationResponse>\n"
    "</SOAP-ENV:Body></SOAP-ENV:Envelope>";

static const char* kVatilonNetwork =
    "<tds:GetNetworkInterfacesResponse>\n<tds:NetworkInterfaces token=\"eth0\">\n<tt:Enabled>true</tt:Enabled>\n"
    "<tt:Info>\n<tt:Name>eth0</tt:Name>\n<tt:HwAddress>e8:b7:23:00:00:01</tt:HwAddress>\n</tt:Info>\n"
    "<tt:IPv4>\n<tt:Enabled>true</tt:Enabled>\n<tt:Config>\n<tt:Manual>\n<tt:Address>192.168.8.148</tt:Address>\n"
    "<tt:PrefixLength>24</tt:PrefixLength>\n</tt:Manual>\n<tt:FromDHCP>\n<tt:Address>192.168.8.200</tt:Address>\n"
    "<tt:PrefixLength>24</tt:PrefixLength>\n</tt:FromDHCP>\n<tt:DHCP>false</tt:DHCP>\n</tt:Config>\n</tt:IPv4>\n"
    "</tds:NetworkInterfaces>\n</tds:GetNetworkInterfacesResponse>";

static const char* kReolinkNetwork =
    "<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"eth0\"><tt:Enabled>true</tt:Enabled>"
    "<tt:Info><tt:Name>eth0</tt:Name><tt:HwAddress>ec:71:db:00:00:02</tt:HwAddress><tt:MTU>0</tt:MTU></tt:Info>"
    "<tt:Link><tt:AdminSettings><tt:AutoNegotiation>false</tt:AutoNegotiation><tt:Speed>10</tt:Speed>"
    "<tt:Duplex>Full</tt:Duplex></tt:AdminSettings><tt:OperSettings><tt:AutoNegotiation>false</tt:AutoNegotiation>"
    "<tt:Speed>10</tt:Speed><tt:Duplex>Full</tt:Duplex></tt:OperSettings><tt:InterfaceType>0</tt:InterfaceType>"
    "</tt:Link><tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:Manual><tt:Address>192.168.8.104</tt:Address>"
    "<tt:PrefixLength>24</tt:PrefixLength></tt:Manual><tt:DHCP>false</tt:DHCP></tt:Config></tt:IPv4>"
    "</tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>";

static const char* kDhcpNetwork =
    "<tds:NetworkInterfaces token=\"eth0\"><tt:Info><tt:HwAddress>AA:BB:CC:00:00:03</tt:HwAddress></tt:Info>"
    "<tt:IPv4><tt:Config><tt:FromDHCP><tt:Address>192.168.8.50</tt:Address><tt:PrefixLength>24</tt:PrefixLength>"
    "</tt:FromDHCP><tt:DHCP>true</tt:DHCP></tt:Config></tt:IPv4></tds:NetworkInterfaces>";

static const char* kReolinkScopes =
    "<tds:GetScopesResponse><tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef>"
    "<tt:ScopeItem>onvif://www.onvif.org/type/video_encoder</tt:ScopeItem></tds:Scopes>"
    "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef>"
    "<tt:ScopeItem>onvif://www.onvif.org/location/country/china</tt:ScopeItem></tds:Scopes>"
    "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/IPC-BO</tt:ScopeItem></tds:Scopes>"
    "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef>"
    "<tt:ScopeItem>onvif://www.onvif.org/hardware/RLC-520A</tt:ScopeItem></tds:Scopes>"
    "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/IPC</tt:ScopeItem></tds:Scopes>"
    "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/IPC</tt:ScopeItem></tds:Scopes>"
    "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef><tt:ScopeItem>odm:name:PortariaEsq</tt:ScopeItem></tds:Scopes>"
    "</tds:GetScopesResponse>";

static const char* kTime =
    "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>NTP</tt:DateTimeType>"
    "<tt:DaylightSavings>false</tt:DaylightSavings><tt:TimeZone><tt:TZ>GMT0</tt:TZ></tt:TimeZone>"
    "<tt:UTCDateTime><tt:Time><tt:Hour>9</tt:Hour><tt:Minute>49</tt:Minute><tt:Second>30</tt:Second></tt:Time>"
    "<tt:Date><tt:Year>2026</tt:Year><tt:Month>11</tt:Month><tt:Day>8</tt:Day></tt:Date></tt:UTCDateTime>"
    "<tt:LocalDateTime><tt:Time><tt:Hour>9</tt:Hour><tt:Minute>49</tt:Minute><tt:Second>30</tt:Second></tt:Time>"
    "<tt:Date><tt:Year>2026</tt:Year><tt:Month>11</tt:Month><tt:Day>8</tt:Day></tt:Date></tt:LocalDateTime>"
    "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>";

static const char* kProfiles =
    "<trt:GetProfilesResponse>"
    "<trt:Profiles fixed=\"true\" token=\"MainStreamToken\"><tt:Name>MainStream</tt:Name>"
    "<tt:VideoSourceConfiguration token=\"VideoSourceMain\"><tt:Name>VideoSourceMain</tt:Name>"
    "<tt:Bounds height=\"1080\" width=\"1920\" y=\"0\" x=\"0\"/></tt:VideoSourceConfiguration>"
    "<tt:VideoEncoderConfiguration token=\"VideoMainEncoder\"><tt:Name>VideoMainEncoder</tt:Name>"
    "<tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height>"
    "</tt:Resolution><tt:Quality>80</tt:Quality><tt:RateControl><tt:FrameRateLimit>20</tt:FrameRateLimit>"
    "<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>2173</tt:BitrateLimit></tt:RateControl>"
    "</tt:VideoEncoderConfiguration></trt:Profiles>"
    "<trt:Profiles fixed=\"true\" token=\"jpeg\"><tt:Name>jpegStream</tt:Name>"
    "<tt:VideoEncoderConfiguration token=\"jpeg\"><tt:Name>VideoEncoder_3</tt:Name><tt:Encoding>JPEG</tt:Encoding>"
    "<tt:Resolution><tt:Width>640</tt:Width><tt:Height>360</tt:Height></tt:Resolution>"
    "<tt:RateControl><tt:FrameRateLimit>1</tt:FrameRateLimit></tt:RateControl>"
    "</tt:VideoEncoderConfiguration></trt:Profiles>"
    "<trt:Profiles token=\"audioOnly\"><tt:Name>AudioOnly</tt:Name></trt:Profiles>"
    "</trt:GetProfilesResponse>";

void test_device_information(void) {
  CameraHardwareInfo info;
  parseDeviceInformation(kVatilonDeviceInfo, info);
  TEST_ASSERT_EQUAL_STRING("Vatilon", info.manufacturer.c_str());
  TEST_ASSERT_EQUAL_STRING("H80", info.model.c_str());
  TEST_ASSERT_EQUAL_STRING("V1.16.25-20250614", info.firmwareVersion.c_str());
  TEST_ASSERT_EQUAL_STRING("0011223344556677", info.serialNumber.c_str());
  TEST_ASSERT_EQUAL_STRING("H80", info.hardwareId.c_str());
}

void test_static_address_comes_from_manual_not_dhcp(void) {
  CameraHardwareInfo info;
  parseNetworkInterfaces(kVatilonNetwork, info);
  TEST_ASSERT_EQUAL_STRING("E8:B7:23:00:00:01", info.macAddress.c_str());
  TEST_ASSERT_EQUAL_STRING("192.168.8.148/24", info.ipv4.c_str());
  TEST_ASSERT_EQUAL_STRING("static", info.ipMode.c_str());
  TEST_ASSERT_EQUAL_STRING("", info.link.c_str()); // no Link element
}

void test_dhcp_address_and_link_speed(void) {
  CameraHardwareInfo info;
  parseNetworkInterfaces(kDhcpNetwork, info);
  TEST_ASSERT_EQUAL_STRING("192.168.8.50/24", info.ipv4.c_str());
  TEST_ASSERT_EQUAL_STRING("DHCP", info.ipMode.c_str());

  CameraHardwareInfo dashed;
  parseNetworkInterfaces("<tds:NetworkInterfaces><tt:HwAddress>58-04-4f-00-00-04</tt:HwAddress></tds:NetworkInterfaces>", dashed);
  TEST_ASSERT_EQUAL_STRING("58:04:4F:00:00:04", dashed.macAddress.c_str());

  CameraHardwareInfo reolink;
  parseNetworkInterfaces(kReolinkNetwork, reolink);
  TEST_ASSERT_EQUAL_STRING("10 Mbit/s, full duplex", reolink.link.c_str());
  TEST_ASSERT_EQUAL_STRING("192.168.8.104/24", reolink.ipv4.c_str());
}

void test_scopes_are_decoded_and_deduplicated(void) {
  CameraHardwareInfo info;
  parseScopes(kReolinkScopes, info);
  TEST_ASSERT_EQUAL_STRING("IPC-BO, IPC", info.scopeName.c_str());
  TEST_ASSERT_EQUAL_STRING("country/china", info.scopeLocation.c_str());
  TEST_ASSERT_EQUAL_STRING("RLC-520A", info.scopeHardware.c_str());

  CameraHardwareInfo spaced;
  parseScopes("<tt:ScopeItem>onvif://www.onvif.org/name/Front%20Door</tt:ScopeItem>", spaced);
  TEST_ASSERT_EQUAL_STRING("Front Door", spaced.scopeName.c_str());
}

void test_clock(void) {
  CameraHardwareInfo info;
  parseSystemDateAndTime(kTime, info);
  TEST_ASSERT_EQUAL_STRING("NTP", info.timeSource.c_str());
  TEST_ASSERT_EQUAL_STRING("GMT0", info.timeZone.c_str());
  // 2026-11-08 09:49:30 UTC
  TEST_ASSERT_TRUE(info.clockUtcEpoch == 1794131370LL);
}

void test_epoch_known_dates(void) {
  TEST_ASSERT_TRUE(utcEpochSeconds(1970, 1, 1, 0, 0, 0) == 0);
  TEST_ASSERT_TRUE(utcEpochSeconds(2000, 3, 1, 0, 0, 0) == 951868800LL);
  TEST_ASSERT_TRUE(utcEpochSeconds(2024, 2, 29, 12, 0, 0) == 1709208000LL);
}

void test_video_streams_use_the_profile_name(void) {
  CameraHardwareInfo info;
  parseVideoStreams(kProfiles, info);
  TEST_ASSERT_EQUAL(2, (int)info.videoStreams.size()); // the audio-only profile has no video
  TEST_ASSERT_EQUAL_STRING("MainStream: H264 1920x1080, 20 fps, 2173 kbit/s", info.videoStreams[0].c_str());
  TEST_ASSERT_EQUAL_STRING("jpegStream: JPEG 640x360, 1 fps", info.videoStreams[1].c_str());
}

void test_missing_or_garbage_responses_leave_fields_empty(void) {
  CameraHardwareInfo info;
  parseDeviceInformation("", info);
  parseNetworkInterfaces("<html>error</html>", info);
  parseScopes("", info);
  parseSystemDateAndTime("<tds:GetSystemDateAndTimeResponse/>", info);
  parseVideoStreams("", info);
  TEST_ASSERT_EQUAL_STRING("", info.model.c_str());
  TEST_ASSERT_EQUAL_STRING("", info.macAddress.c_str());
  TEST_ASSERT_EQUAL_STRING("", info.ipMode.c_str());
  TEST_ASSERT_TRUE(info.clockUtcEpoch == 0);
  TEST_ASSERT_EQUAL(0, (int)info.videoStreams.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_device_information);
  RUN_TEST(test_static_address_comes_from_manual_not_dhcp);
  RUN_TEST(test_dhcp_address_and_link_speed);
  RUN_TEST(test_scopes_are_decoded_and_deduplicated);
  RUN_TEST(test_clock);
  RUN_TEST(test_epoch_known_dates);
  RUN_TEST(test_video_streams_use_the_profile_name);
  RUN_TEST(test_missing_or_garbage_responses_leave_fields_empty);
  return UNITY_END();
}
