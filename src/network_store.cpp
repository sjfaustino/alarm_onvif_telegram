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
static const char* DEFAULT_HOSTNAME = "cameramonitor";

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
  }
  prefs.end();

  if (!alreadyInitialized) {
    creds.primary.ssid = WIFI_SSID;
    creds.primary.password = WIFI_PASSWORD;
    creds.hostname = DEFAULT_HOSTNAME;
    saveWifiCredentials(creds);
    Serial.println("[network_store] First boot with NVS-backed WiFi config - seeded primary from "
                    "secrets.h's WIFI_SSID/WIFI_PASSWORD, default hostname \"" +
                    String(DEFAULT_HOSTNAME) + "\", no backup network, DHCP.");
  }
  return creds;
}

bool saveWifiCredentials(const WifiCredentials& creds) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  prefs.putString(NVS_KEY_SSID, creds.primary.ssid);
  prefs.putString(NVS_KEY_PASS, creds.primary.password);
  prefs.putString(NVS_KEY_SSID2, creds.backup.ssid);
  prefs.putString(NVS_KEY_PASS2, creds.backup.password);
  prefs.putString(NVS_KEY_HOST, creds.hostname);
  prefs.putBool(NVS_KEY_STATIC, creds.useStaticIP);
  prefs.putString(NVS_KEY_IP, creds.staticIP);
  prefs.putString(NVS_KEY_SUBNET, creds.staticSubnet);
  prefs.putString(NVS_KEY_GW, creds.staticGateway);
  prefs.putString(NVS_KEY_DNS, creds.staticDNS);
  prefs.end();
  return true;
}
