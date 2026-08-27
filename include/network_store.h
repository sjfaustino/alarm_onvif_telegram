#pragma once
#include <Arduino.h>

struct WifiNetwork {
  String ssid;
  String password;
};

// WiFi credentials + mDNS hostname - persisted in NVS (Preferences,
// namespace "netcfg"), editable at runtime from the web UI's Network
// section instead of only at compile time via secrets.h. A change here
// takes effect after a reboot, same as camera changes - not applied live,
// since a wrong SSID/password would drop the board off the network with no
// way back to the web UI to fix it short of physical/serial access.
struct WifiCredentials {
  WifiNetwork primary;

  // Optional - empty ssid means no backup. Tried only if primary doesn't
  // connect in time; if backup connects, connectWiFi() swaps primary/backup
  // and persists it, so future boots try whichever worked first.
  WifiNetwork backup;

  // Reaches the dashboard at http://<hostname>.local instead of the IP.
  // Set once at boot via MDNS.begin(); renaming takes effect after a reboot.
  String hostname;

  // Applied via WiFi.config() before WiFi.begin(), for whichever of
  // primary/backup ends up connecting. useStaticIP=false means DHCP. The
  // four fields are dotted-quad text; staticDNS empty falls back to the gateway.
  bool useStaticIP = false;
  String staticIP;
  String staticSubnet;
  String staticGateway;
  String staticDNS;

  // No port field - ESP32's SNTP client hardcodes UDP port 123.
  String ntpServer;
  unsigned long ntpSyncIntervalMs = 3600000UL; // 1 hour, matches ESP-IDF's own default

  // Optional POSIX TZ rule string (e.g. "WET0WEST,M3.5.0/1,M10.5.0" for
  // mainland Portugal - look yours up at
  // https://github.com/nayarsystems/posix_tz_db), applied at boot. Only
  // affects DST-aware local-time *display* (telegram.cpp's alert photo
  // captions); the system clock itself always stays true UTC regardless -
  // WS-Security's Created timestamp reads UTC directly via gmtime_r() and
  // ignores this entirely. Empty (default) means captions stay in UTC.
  String posixTz;
};

// Loads WiFi credentials + hostname + NTP config from NVS. On the very
// first boot (nothing in NVS yet), seeds primary ssid/password from
// secrets.h, hostname "cameramonitor", NTP "pool.ntp.org" (1h resync).
WifiCredentials loadWifiCredentials();

// Overwrites the persisted WiFi credentials + hostname. Pass the existing
// value for any field to leave it unchanged.
bool saveWifiCredentials(const WifiCredentials& creds);
