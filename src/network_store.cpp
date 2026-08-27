#include "network_store.h"
#include "secrets.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "netcfg";
static const char* NVS_KEY_SSID   = "ssid";  // primary
static const char* NVS_KEY_PASS   = "pass";  // primary
static const char* NVS_KEY_SSID2  = "ssid2"; // backup
static const char* NVS_KEY_PASS2  = "pass2"; // backup
static const char* NVS_KEY_HOST   = "host";
static const char* NVS_KEY_STATIC = "static";
static const char* NVS_KEY_IP     = "ip";
static const char* NVS_KEY_SUBNET = "subnet";
static const char* NVS_KEY_GW     = "gw";
static const char* NVS_KEY_DNS    = "dns";
static const char* NVS_KEY_NTPSRV = "ntpsrv";
static const char* NVS_KEY_NTPINT = "ntpint";
static const char* NVS_KEY_TZ     = "posixtz";
static const char* DEFAULT_HOSTNAME = "cameramonitor";
static const char* DEFAULT_NTP_SERVER = "pool.ntp.org";

WifiCredentials loadWifiCredentials() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  bool alreadyInitialized = prefs.isKey(NVS_KEY_SSID);
  WifiCredentials creds;
  if (alreadyInitialized) {
    creds.primary.ssid     = prefs.getString(NVS_KEY_SSID, "");
    creds.primary.password = prefs.getString(NVS_KEY_PASS, "");
    // ssid2/pass2/static/ip/subnet/gw/dns didn't exist before backup
    // networks and static IP were added - absent keys just come back
    // empty/false, meaning "no backup, DHCP", the correct default for
    // anyone upgrading from before these existed.
    creds.backup.ssid      = prefs.getString(NVS_KEY_SSID2, "");
    creds.backup.password  = prefs.getString(NVS_KEY_PASS2, "");
    creds.hostname          = prefs.getString(NVS_KEY_HOST, DEFAULT_HOSTNAME);
    creds.useStaticIP      = prefs.getBool(NVS_KEY_STATIC, false);
    creds.staticIP          = prefs.getString(NVS_KEY_IP, "");
    creds.staticSubnet      = prefs.getString(NVS_KEY_SUBNET, "");
    creds.staticGateway    = prefs.getString(NVS_KEY_GW, "");
    creds.staticDNS         = prefs.getString(NVS_KEY_DNS, "");
    creds.ntpServer         = prefs.getString(NVS_KEY_NTPSRV, DEFAULT_NTP_SERVER);
    creds.ntpSyncIntervalMs = prefs.getULong(NVS_KEY_NTPINT, 3600000UL);
    // Absent key (didn't exist before this field was added) correctly
    // defaults to "" - no TZ applied, display stays UTC - same
    // backward-compat reasoning as ssid2/pass2/static/etc above.
    creds.posixTz = prefs.getString(NVS_KEY_TZ, "");
  }
  prefs.end();

  if (!alreadyInitialized) {
    creds.primary.ssid = WIFI_SSID;
    creds.primary.password = WIFI_PASSWORD;
    creds.hostname = DEFAULT_HOSTNAME;
    creds.ntpServer = DEFAULT_NTP_SERVER;
    bool seeded = saveWifiCredentials(creds);
    Serial.println(seeded
        ? "[network_store] First boot with NVS-backed WiFi config - seeded primary from "
          "secrets.h's WIFI_SSID/WIFI_PASSWORD, default hostname \"" + String(DEFAULT_HOSTNAME) +
          "\", NTP server \"" + String(DEFAULT_NTP_SERVER) + "\" (1h resync), no backup network, DHCP."
        : "[network_store] ERROR: failed to persist the first-boot WiFi config seed to NVS - "
          "this boot will still use it, but it wasn't saved and won't survive a reboot.");
  }
  return creds;
}

// putString returns bytes written, 0 on failure - comparing against the
// source string's own length (rather than just "> 0") correctly treats a
// legitimately empty field (no backup network, DHCP, no TZ, etc. - several
// of these fields are optional and empty by design) as success too, not
// just a non-empty one. Same failure class camera_store.cpp's saveCameras
// hit in the field (NVS full/write error silently ignored) - here it means
// the WiFi config the caller believes it just set was never actually
// persisted, and reverts to whatever's really on flash on the next reboot.
static bool putStringChecked(Preferences& prefs, const char* key, const String& value) {
  return prefs.putString(key, value) == value.length();
}

bool saveWifiCredentials(const WifiCredentials& creds) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = true;
  ok &= putStringChecked(prefs, NVS_KEY_SSID, creds.primary.ssid);
  ok &= putStringChecked(prefs, NVS_KEY_PASS, creds.primary.password);
  ok &= putStringChecked(prefs, NVS_KEY_SSID2, creds.backup.ssid);
  ok &= putStringChecked(prefs, NVS_KEY_PASS2, creds.backup.password);
  ok &= putStringChecked(prefs, NVS_KEY_HOST, creds.hostname);
  ok &= prefs.putBool(NVS_KEY_STATIC, creds.useStaticIP) > 0;
  ok &= putStringChecked(prefs, NVS_KEY_IP, creds.staticIP);
  ok &= putStringChecked(prefs, NVS_KEY_SUBNET, creds.staticSubnet);
  ok &= putStringChecked(prefs, NVS_KEY_GW, creds.staticGateway);
  ok &= putStringChecked(prefs, NVS_KEY_DNS, creds.staticDNS);
  ok &= putStringChecked(prefs, NVS_KEY_NTPSRV, creds.ntpServer);
  ok &= prefs.putULong(NVS_KEY_NTPINT, creds.ntpSyncIntervalMs) > 0;
  ok &= putStringChecked(prefs, NVS_KEY_TZ, creds.posixTz);
  prefs.end();
  if (!ok) {
    Serial.println("[network_store] ERROR: failed to persist WiFi config to NVS - NVS may be full. "
                    "The in-memory config changed but NVS still has the old data; this WILL be lost "
                    "on reboot.");
  }
  return ok;
}
