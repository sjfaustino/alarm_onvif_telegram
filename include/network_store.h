#pragma once
#include <Arduino.h>

// WiFi credentials - persisted in NVS (Preferences, namespace "netcfg"),
// editable at runtime from the web UI's Network section instead of only at
// compile time via secrets.h. A change here takes effect after a reboot,
// same as camera changes - not applied live, since a wrong SSID/password
// would drop the board off the network with no way back to the web UI to
// fix it short of physical/serial access.
struct WifiCredentials {
  String ssid;
  String password;
};

// Loads WiFi credentials from NVS. On the very first boot after upgrading
// to this (nothing in NVS yet), seeds them from secrets.h's WIFI_SSID/
// WIFI_PASSWORD so the board keeps connecting exactly as before until
// changed via the web UI.
WifiCredentials loadWifiCredentials();

// Overwrites the persisted WiFi credentials. Pass the existing value for
// either field to leave it unchanged (used by the web UI's "leave blank to
// keep current" pattern for the password field).
bool saveWifiCredentials(const WifiCredentials& creds);
