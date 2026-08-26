#include "network_store.h"
#include "secrets.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "netcfg";
static const char* NVS_KEY_SSID  = "ssid";  // primary
static const char* NVS_KEY_PASS  = "pass";  // primary
static const char* NVS_KEY_SSID2 = "ssid2"; // backup
static const char* NVS_KEY_PASS2 = "pass2"; // backup
static const char* NVS_KEY_HOST  = "host";
static const char* DEFAULT_HOSTNAME = "cameramonitor";

WifiCredentials loadWifiCredentials() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  bool alreadyInitialized = prefs.isKey(NVS_KEY_SSID);
  WifiCredentials creds;
  if (alreadyInitialized) {
    creds.primary.ssid     = prefs.getString(NVS_KEY_SSID, "");
    creds.primary.password = prefs.getString(NVS_KEY_PASS, "");
    // ssid2/pass2 didn't exist before backup networks were added - absent
    // keys just come back empty, meaning "no backup configured", which is
    // the correct default for anyone upgrading from before this existed.
    creds.backup.ssid      = prefs.getString(NVS_KEY_SSID2, "");
    creds.backup.password  = prefs.getString(NVS_KEY_PASS2, "");
    creds.hostname          = prefs.getString(NVS_KEY_HOST, DEFAULT_HOSTNAME);
  }
  prefs.end();

  if (!alreadyInitialized) {
    creds.primary.ssid = WIFI_SSID;
    creds.primary.password = WIFI_PASSWORD;
    creds.hostname = DEFAULT_HOSTNAME;
    saveWifiCredentials(creds);
    Serial.println("[network_store] First boot with NVS-backed WiFi config - seeded primary from "
                    "secrets.h's WIFI_SSID/WIFI_PASSWORD, default hostname \"" +
                    String(DEFAULT_HOSTNAME) + "\", no backup network configured.");
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
  prefs.end();
  return true;
}
