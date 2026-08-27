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

  // Display-only offset from UTC, in minutes (can be negative; supports
  // half/quarter-hour zones like UTC+5:30). The board's actual system
  // clock stays UTC always - main.cpp's setupTime() calls configTime(0, 0,
  // ...) unconditionally - because ONVIF's WS-Security Created timestamp
  // (isoTimeNow() in onvif_soap.cpp) must be true UTC for cameras to accept
  // it; shifting the system clock itself would silently break that. This
  // offset is applied only where a human reads a wall-clock time, e.g.
  // telegram.cpp's nowTimestampString() for alert photo captions. Default 0
  // (UTC) - today's behavior unchanged until set.
  int timezoneOffsetMinutes = 0;
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
