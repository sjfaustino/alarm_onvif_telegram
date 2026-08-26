#include "network_store.h"
#include "secrets.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "netcfg";
static const char* NVS_KEY_SSID  = "ssid";
static const char* NVS_KEY_PASS  = "pass";

WifiCredentials loadWifiCredentials() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  bool alreadyInitialized = prefs.isKey(NVS_KEY_SSID);
  WifiCredentials creds;
  if (alreadyInitialized) {
    creds.ssid = prefs.getString(NVS_KEY_SSID, "");
    creds.password = prefs.getString(NVS_KEY_PASS, "");
  }
  prefs.end();

  if (!alreadyInitialized) {
    creds.ssid = WIFI_SSID;
    creds.password = WIFI_PASSWORD;
    saveWifiCredentials(creds);
    Serial.println("[network_store] First boot with NVS-backed WiFi config - seeded from "
                    "secrets.h's WIFI_SSID/WIFI_PASSWORD.");
  }
  return creds;
}

bool saveWifiCredentials(const WifiCredentials& creds) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  prefs.putString(NVS_KEY_SSID, creds.ssid);
  prefs.putString(NVS_KEY_PASS, creds.password);
  prefs.end();
  return true;
}
