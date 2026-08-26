#include "network_store.h"
#include "secrets.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "netcfg";
static const char* NVS_KEY_SSID  = "ssid";
static const char* NVS_KEY_PASS  = "pass";
static const char* NVS_KEY_HOST  = "host";
static const char* DEFAULT_HOSTNAME = "cameramonitor";

WifiCredentials loadWifiCredentials() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  bool alreadyInitialized = prefs.isKey(NVS_KEY_SSID);
  WifiCredentials creds;
  if (alreadyInitialized) {
    creds.ssid = prefs.getString(NVS_KEY_SSID, "");
    creds.password = prefs.getString(NVS_KEY_PASS, "");
    creds.hostname = prefs.getString(NVS_KEY_HOST, DEFAULT_HOSTNAME);
  }
  prefs.end();

  if (!alreadyInitialized) {
    creds.ssid = WIFI_SSID;
    creds.password = WIFI_PASSWORD;
    creds.hostname = DEFAULT_HOSTNAME;
    saveWifiCredentials(creds);
    Serial.println("[network_store] First boot with NVS-backed WiFi config - seeded from "
                    "secrets.h's WIFI_SSID/WIFI_PASSWORD, default hostname \"" +
                    String(DEFAULT_HOSTNAME) + "\".");
  }
  return creds;
}

bool saveWifiCredentials(const WifiCredentials& creds) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  prefs.putString(NVS_KEY_SSID, creds.ssid);
  prefs.putString(NVS_KEY_PASS, creds.password);
  prefs.putString(NVS_KEY_HOST, creds.hostname);
  prefs.end();
  return true;
}
