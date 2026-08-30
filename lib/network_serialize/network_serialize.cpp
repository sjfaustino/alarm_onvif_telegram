#include "network_serialize.h"
#include <vector>

// Same non-printable ASCII separator as camera_serialize.cpp/
// telegram_user_serialize.cpp - no SSID, hostname, or address field should
// ever legitimately contain it, so records are just split/joined on it
// rather than building full field-escaping machinery.
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

// Always emits NETWORK_SCHEMA_VERSION's layout - one field order, ever. A
// layout change means bumping the version and writing a new
// deserializeNetworkConfigV1-style branch, not editing this one in place.
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

// Version 1 (NETWORK_SCHEMA_VERSION): the only layout so far - requires an
// exact field count, same reasoning as camera_serialize.cpp's
// deserializeCameraV1/V2 (no historical pre-versioning format to be
// tolerant of here, unlike cameras/users - this serializer is new).
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

  // Unknown version, newer than anything this firmware knows about (most
  // likely: downgraded after a later firmware version changed the layout).
  // Best-effort fall through to the newest known layout, same as
  // camera_serialize.cpp/telegram_user_serialize.cpp do.
  return deserializeNetworkConfigV1(fields);
}
