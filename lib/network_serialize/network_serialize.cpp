#include "network_serialize.h"
#include <vector>

// Same field separator as the other serializers.
static const char FIELD_SEP = '\x1F';

static String stripSeparators(const String& s) {
  String out = s;
  out.replace(String(FIELD_SEP), "");
  return out;
}

static std::vector<String> splitFields(const String& record) {
  std::vector<String> fields;
  int fieldStart = 0;
  for (int i = 0; i <= (int)record.length(); i++) {
    if (i == (int)record.length() || record[i] == FIELD_SEP) {
      fields.push_back(record.substring(fieldStart, i));
      fieldStart = i + 1;
    }
  }
  return fields;
}

// One layout per version, never edited.
String serializeNetworkConfig(const WifiCredentials& creds) {
  String s;
  s += stripSeparators(creds.primary.ssid);   s += FIELD_SEP;
  s += stripSeparators(creds.backup.ssid);    s += FIELD_SEP;
  s += stripSeparators(creds.hostname);       s += FIELD_SEP;
  s += (creds.useStaticIP ? "1" : "0");       s += FIELD_SEP;
  s += stripSeparators(creds.staticIP);       s += FIELD_SEP;
  s += stripSeparators(creds.staticSubnet);   s += FIELD_SEP;
  s += stripSeparators(creds.staticGateway);  s += FIELD_SEP;
  s += stripSeparators(creds.staticDNS);      s += FIELD_SEP;
  s += stripSeparators(creds.ntpServer);      s += FIELD_SEP;
  s += String(creds.ntpSyncIntervalMs);       s += FIELD_SEP;
  s += stripSeparators(creds.posixTz);
  return s;
}

// V1: the only layout; exact field count.
static WifiCredentials deserializeNetworkConfigV1(const std::vector<String>& fields) {
  WifiCredentials creds;
  if (fields.size() != 11) return creds; // malformed - caller treats hostname=="" as "not found"

  creds.primary.ssid    = fields[0];
  creds.backup.ssid     = fields[1];
  creds.hostname        = fields[2];
  creds.useStaticIP     = fields[3] == "1";
  creds.staticIP        = fields[4];
  creds.staticSubnet    = fields[5];
  creds.staticGateway   = fields[6];
  creds.staticDNS       = fields[7];
  creds.ntpServer       = fields[8];
  if (fields[9].length() > 0) creds.ntpSyncIntervalMs = (unsigned long)fields[9].toInt();
  creds.posixTz          = fields[10];
  return creds;
}

WifiCredentials deserializeNetworkConfig(const String& record, uint16_t recordVersion) {
  std::vector<String> fields = splitFields(record);

  if (recordVersion == NETWORK_SCHEMA_VERSION) return deserializeNetworkConfigV1(fields);

  // Unknown/newer version: try the newest layout.
  return deserializeNetworkConfigV1(fields);
}
