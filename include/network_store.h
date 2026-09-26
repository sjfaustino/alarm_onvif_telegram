#pragma once
#include <Arduino.h>

struct WifiNetwork {
  String ssid;
  String password;
};

// WiFi, hostname and NTP settings, persisted in NVS ("netcfg"). Changes apply
// after a reboot - applying a wrong password live would strand the board.
struct WifiCredentials {
  WifiNetwork primary;

  // Optional (empty ssid = none). If only backup connects, the two are swapped
  // and saved.
  WifiNetwork backup;

  // http://<hostname>.local; applied at boot.
  String hostname;

  // Static IP for whichever network connects; false = DHCP. Empty staticDNS
  // uses the gateway.
  bool useStaticIP = false;
  String staticIP;
  String staticSubnet;
  String staticGateway;
  String staticDNS;

  // No port: ESP32's SNTP client always uses 123.
  String ntpServer;
  unsigned long ntpSyncIntervalMs = 3600000UL; // 1 hour, matches ESP-IDF's own default

  // Optional POSIX TZ rule (e.g. "WET0WEST,M3.5.0/1,M10.5.0"; see
  // github.com/nayarsystems/posix_tz_db). Only affects displayed local times;
  // the system clock stays UTC. Empty = UTC.
  String posixTz;
};

// Loads from NVS; a fresh board seeds from secrets.h (hostname
// "cameramonitor", NTP pool.ntp.org, 1h resync).
WifiCredentials loadWifiCredentials();

bool saveWifiCredentials(const WifiCredentials& creds);

// Keeps only letters, digits and hyphens (mDNS-safe). Also applied at
// MDNS.begin(), since imported configs bypass the form.
String sanitizeHostname(const String& raw);
