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

  // Optional - backup.ssid empty means no backup is configured. Tried by
  // main.cpp's connectWiFi() only if primary doesn't connect within its
  // timeout. If backup connects, connectWiFi() swaps primary/backup and
  // persists the swap via saveWifiCredentials(), so future boots try
  // whichever network actually worked first.
  WifiNetwork backup;

  // mDNS hostname - lets the dashboard be reached at http://<hostname>.local
  // instead of the board's IP. Set once at boot via MDNS.begin() in
  // main.cpp; renaming it here only takes effect after a reboot too.
  String hostname;

  // Static IP config - applied via WiFi.config() before WiFi.begin() in
  // main.cpp's connectWiFi(), regardless of whether primary or backup ends
  // up connecting (a static setup is typically two APs sharing one
  // LAN/subnet). useStaticIP=false (default) means normal DHCP. The four
  // String fields are dotted-quad text, parsed into IPAddress at connect
  // time - staticDNS may be left empty, in which case connectWiFi() falls
  // back to using the gateway as the DNS server.
  bool useStaticIP = false;
  String staticIP;
  String staticSubnet;
  String staticGateway;
  String staticDNS;

  // NTP server + resync interval, applied in main.cpp's setupTime() via
  // configTime()/esp_sntp_set_sync_interval(). No port field - ESP32's
  // SNTP client (Arduino's configTime() and the underlying lwIP SNTP
  // implementation) hardcodes the standard NTP UDP port 123; there's no
  // supported way to point it at a different port.
  String ntpServer;
  unsigned long ntpSyncIntervalMs = 3600000UL; // 1 hour, matches ESP-IDF's own default

  // Optional POSIX TZ rule string (e.g. "WET0WEST,M3.5.0/1,M10.5.0" for
  // mainland Portugal - look yours up at
  // https://github.com/nayarsystems/posix_tz_db) applied via
  // setenv("TZ",...)/tzset() once at boot, in main.cpp's setupTime(), right
  // after configTime(). Lets DST-aware local time be computed automatically
  // for display purposes - currently just telegram.cpp's
  // nowTimestampString(), used in alert photo captions. Empty (default)
  // means no TZ is applied, so display stays UTC - today's behavior,
  // unchanged until this is set.
  //
  // The board's actual system clock is unaffected either way: ONVIF's
  // WS-Security Created timestamp (isoTimeNow() in onvif_soap.cpp) reads
  // true UTC directly via gmtime_r(), which ignores TZ entirely, so cameras
  // always see honest UTC no matter what's configured here. Takes effect
  // after a reboot, like the rest of this struct's fields.
  String posixTz;
};

// Loads WiFi credentials + hostname + NTP config from NVS. On the very
// first boot after upgrading to this (nothing in NVS yet), seeds primary
// ssid/password from secrets.h's WIFI_SSID/WIFI_PASSWORD so the board keeps
// connecting exactly as before until changed via the web UI, leaves backup
// unconfigured, defaults hostname to "cameramonitor", and defaults
// ntpServer to "pool.ntp.org" with a 1-hour resync interval.
WifiCredentials loadWifiCredentials();

// Overwrites the persisted WiFi credentials + hostname. Pass the existing
// value for any field to leave it unchanged (used by the web UI's "leave
// blank to keep current" pattern for password fields).
bool saveWifiCredentials(const WifiCredentials& creds);
