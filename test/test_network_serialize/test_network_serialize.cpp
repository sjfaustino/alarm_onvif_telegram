#include <unity.h>
#include <Arduino.h>
#include "network_serialize.h"

void setUp(void) {}
void tearDown(void) {}

// The real field separator (network_serialize.cpp's private FIELD_SEP) is
// non-printable ASCII 0x1F, not '|' - joins fields with the real character
// so the "malformed record" tests below actually exercise the real format.
static String joinFields(std::initializer_list<const char*> fields) {
  String out;
  bool first = true;
  for (const char* f : fields) {
    if (!first) out += (char)0x1F;
    out += f;
    first = false;
  }
  return out;
}

static WifiCredentials sampleCreds() {
  WifiCredentials c;
  c.primary.ssid = "HomeWiFi";
  c.primary.password = "shouldNeverBeExported";
  c.backup.ssid = "BackupWiFi";
  c.backup.password = "alsoNeverExported";
  c.hostname = "cameramonitor";
  c.useStaticIP = true;
  c.staticIP = "192.168.1.50";
  c.staticSubnet = "255.255.255.0";
  c.staticGateway = "192.168.1.1";
  c.staticDNS = "192.168.1.1";
  c.ntpServer = "pool.ntp.org";
  c.ntpSyncIntervalMs = 3600000UL;
  c.posixTz = "WET0WEST,M3.5.0/1,M10.5.0";
  return c;
}

// A full round trip must reproduce every field except the two passwords -
// this is the "silent NVS corruption" risk directly: if a future field
// gets inserted in the middle of the list instead of appended at the end,
// this is the test that should fail instead of an imported network config
// silently landing in the wrong fields.
void test_round_trip_preserves_every_field_except_passwords(void) {
  WifiCredentials original = sampleCreds();
  WifiCredentials restored =
      deserializeNetworkConfig(serializeNetworkConfig(original), NETWORK_SCHEMA_VERSION);

  TEST_ASSERT_EQUAL_STRING(original.primary.ssid.c_str(), restored.primary.ssid.c_str());
  TEST_ASSERT_EQUAL_STRING(original.backup.ssid.c_str(), restored.backup.ssid.c_str());
  TEST_ASSERT_EQUAL_STRING(original.hostname.c_str(), restored.hostname.c_str());
  TEST_ASSERT_EQUAL(original.useStaticIP, restored.useStaticIP);
  TEST_ASSERT_EQUAL_STRING(original.staticIP.c_str(), restored.staticIP.c_str());
  TEST_ASSERT_EQUAL_STRING(original.staticSubnet.c_str(), restored.staticSubnet.c_str());
  TEST_ASSERT_EQUAL_STRING(original.staticGateway.c_str(), restored.staticGateway.c_str());
  TEST_ASSERT_EQUAL_STRING(original.staticDNS.c_str(), restored.staticDNS.c_str());
  TEST_ASSERT_EQUAL_STRING(original.ntpServer.c_str(), restored.ntpServer.c_str());
  TEST_ASSERT_EQUAL_UINT32(original.ntpSyncIntervalMs, restored.ntpSyncIntervalMs);
  TEST_ASSERT_EQUAL_STRING(original.posixTz.c_str(), restored.posixTz.c_str());

  // Never serialized, on purpose - see serializeNetworkConfig's own comment.
  TEST_ASSERT_EQUAL_STRING("", restored.primary.password.c_str());
  TEST_ASSERT_EQUAL_STRING("", restored.backup.password.c_str());
}

void test_round_trip_with_falsy_flags_and_empty_optionals(void) {
  WifiCredentials c;
  c.primary.ssid = "OnlyPrimary";
  c.hostname = "mycam";
  c.useStaticIP = false;
  c.ntpServer = "pool.ntp.org";
  // backup/staticIP/staticSubnet/staticGateway/staticDNS/posixTz left empty.

  WifiCredentials restored = deserializeNetworkConfig(serializeNetworkConfig(c), NETWORK_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("mycam", restored.hostname.c_str());
  TEST_ASSERT_FALSE(restored.useStaticIP);
  TEST_ASSERT_EQUAL_STRING("", restored.backup.ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("", restored.posixTz.c_str());
}

// config_import_parse.cpp's caller treats an empty hostname as "malformed,
// no valid Network section" - a real saved config always has a hostname
// (the dashboard form requires one), so this is a safe sentinel.
void test_malformed_record_wrong_field_count_returns_empty_hostname(void) {
  WifiCredentials c = deserializeNetworkConfig("only|three|fields", NETWORK_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("", c.hostname.c_str());
}

void test_empty_record_returns_empty_hostname(void) {
  WifiCredentials c = deserializeNetworkConfig("", NETWORK_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("", c.hostname.c_str());
}

// Same "above the exact count, not just below" gap camera_serialize's own
// tests guard against - a `!= 11` -> `< 11` mutation would pass a too-short
// record's rejection unchanged but silently accept a 12-field one.
void test_field_count_above_exact_is_also_rejected(void) {
  String tooMany = joinFields({"ssid", "backupSsid", "host", "0", "", "", "", "", "ntp.org", "3600000",
                                "", "extra"}); // 12 fields
  WifiCredentials c = deserializeNetworkConfig(tooMany, NETWORK_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("", c.hostname.c_str());
}

void test_exact_field_count_is_accepted(void) {
  String exact = joinFields({"ssid", "backupSsid", "host", "1", "1.2.3.4", "255.255.255.0", "1.2.3.1",
                              "8.8.8.8", "ntp.org", "60000", "WET0WEST"}); // 11 fields
  WifiCredentials c = deserializeNetworkConfig(exact, NETWORK_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING("host", c.hostname.c_str());
  TEST_ASSERT_TRUE(c.useStaticIP);
  TEST_ASSERT_EQUAL_UINT32(60000, c.ntpSyncIntervalMs);
}

// A version newer than this build knows about (firmware downgraded after a
// later version changed the layout) falls through to the newest known (V1)
// layout rather than being discarded outright, same as camera_serialize.cpp.
void test_unknown_future_version_falls_back_to_newest_known_layout(void) {
  String record = joinFields({"ssid", "", "host", "0", "", "", "", "", "ntp.org", "3600000", ""});
  WifiCredentials c = deserializeNetworkConfig(record, (uint16_t)(NETWORK_SCHEMA_VERSION + 1));
  TEST_ASSERT_EQUAL_STRING("host", c.hostname.c_str());
}

// An SSID/hostname containing the field separator character must not
// corrupt the record's field count.
void test_field_separator_character_in_input_is_stripped_not_corrupting(void) {
  WifiCredentials c = sampleCreds();
  c.hostname = String("line") + (char)0x1F + "break";
  WifiCredentials restored = deserializeNetworkConfig(serializeNetworkConfig(c), NETWORK_SCHEMA_VERSION);
  TEST_ASSERT_EQUAL_STRING(c.primary.ssid.c_str(), restored.primary.ssid.c_str()); // fields still line up
  TEST_ASSERT_EQUAL_STRING("linebreak", restored.hostname.c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_preserves_every_field_except_passwords);
  RUN_TEST(test_round_trip_with_falsy_flags_and_empty_optionals);
  RUN_TEST(test_malformed_record_wrong_field_count_returns_empty_hostname);
  RUN_TEST(test_empty_record_returns_empty_hostname);
  RUN_TEST(test_field_count_above_exact_is_also_rejected);
  RUN_TEST(test_exact_field_count_is_accepted);
  RUN_TEST(test_unknown_future_version_falls_back_to_newest_known_layout);
  RUN_TEST(test_field_separator_character_in_input_is_stripped_not_corrupting);
  return UNITY_END();
}
