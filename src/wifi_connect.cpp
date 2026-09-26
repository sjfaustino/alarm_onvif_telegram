#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "wifi_connect.h"
#include "backoff.h"
#include "network_store.h"

WifiCredentials g_wifiCredentials;


static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000UL;

// Extra idle time between reconnect attempts, on top of connectWiFi()'s own
// ~60s (30s primary + 30s backup). Doubles per consecutive failure up to
// WIFI_RETRY_BACKOFF_MAX_MS; resets to 0 on success - avoids retrying both
// networks at a fixed cadence for the whole length of a multi-hour outage.
static const unsigned long WIFI_RETRY_BACKOFF_START_MS = 10000UL;  // extra wait after the 1st consecutive failure
static const unsigned long WIFI_RETRY_BACKOFF_MAX_MS   = 300000UL; // cap: 5 minutes between attempts
static uint8_t g_wifiFailureStreak = 0;
static unsigned long g_wifiRetryDelayMs = 0;
static unsigned long g_wifiRetryDueMs = 0; // millis() timestamp; next connectWiFi() attempt is due once reached

// Attempts one network, blocking up to timeoutMs. Returns whether it connected.
static bool tryConnectWiFi(const WifiNetwork& net, unsigned long timeoutMs) {
  if (net.ssid.length() == 0) return false;
  Serial.printf("\nConnecting to WiFi \"%s\"...\n", net.ssid.c_str());
  WiFi.begin(net.ssid.c_str(), net.password.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
    esp_task_wdt_reset();
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// Applies the stored static IP config, if enabled - must run after
// WiFi.mode(WIFI_STA) but before WiFi.begin(). Same config applies
// regardless of which network (primary/backup) ends up connecting. Falls
// back to DHCP if the stored values don't parse, rather than failing to
// connect at all over a config typo.
static void applyStaticIpConfig() {
  if (!g_wifiCredentials.useStaticIP) return;

  IPAddress ip, subnet, gateway, dns;
  bool ok = ip.fromString(g_wifiCredentials.staticIP) &&
            subnet.fromString(g_wifiCredentials.staticSubnet) &&
            gateway.fromString(g_wifiCredentials.staticGateway);
  if (!ok) {
    Serial.println("WARNING: static IP config incomplete/invalid - falling back to DHCP.");
    return;
  }
  if (g_wifiCredentials.staticDNS.length() == 0 || !dns.fromString(g_wifiCredentials.staticDNS)) {
    dns = gateway; // no DNS configured (or it doesn't parse) - the gateway usually doubles as one on a home LAN
  }

  WiFi.config(ip, gateway, subnet, dns);
  Serial.printf("Static IP: %s  gateway: %s  subnet: %s  DNS: %s\n",
                ip.toString().c_str(), gateway.toString().c_str(),
                subnet.toString().c_str(), dns.toString().c_str());
}

// Tries primary, then backup (if configured) on failure. If backup is what
// worked, it's promoted to primary and persisted, so future boots try
// whichever network is actually reachable first.
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  applyStaticIpConfig();

  if (tryConnectWiFi(g_wifiCredentials.primary, WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
    return;
  }

  if (g_wifiCredentials.backup.ssid.length() > 0) {
    Serial.println("Primary WiFi not reachable - trying backup...");
    if (tryConnectWiFi(g_wifiCredentials.backup, WIFI_CONNECT_TIMEOUT_MS)) {
      Serial.print("ESP32 IP: ");
      Serial.println(WiFi.localIP());
      Serial.println("Backup WiFi connected - promoting it to primary for future boots.");
      WifiNetwork oldPrimary = g_wifiCredentials.primary;
      g_wifiCredentials.primary = g_wifiCredentials.backup;
      g_wifiCredentials.backup = oldPrimary;
      if (!saveWifiCredentials(g_wifiCredentials)) {
        Serial.println("ERROR: failed to persist the promoted backup network to NVS - "
                        "this boot will still use it, but it may try primary first again after a reboot.");
      }
      return;
    }
  }

  Serial.println("ERROR: WiFi connection failed (primary" +
                  String(g_wifiCredentials.backup.ssid.length() > 0 ? " and backup" : "") + ").");
}

bool wifiReconnectDue() {
  return (long)(millis() - g_wifiRetryDueMs) >= 0;
}

void recordWifiReconnectResult(bool connected) {
  if (connected) {
    g_wifiFailureStreak = 0;
    g_wifiRetryDelayMs = 0;
    return;
  }
  g_wifiRetryDelayMs = nextBackoffDelayMs(g_wifiRetryDelayMs, WIFI_RETRY_BACKOFF_START_MS,
                                          WIFI_RETRY_BACKOFF_MAX_MS);
  g_wifiFailureStreak++;
  g_wifiRetryDueMs = millis() + g_wifiRetryDelayMs;
  Serial.printf("WiFi still down after %u consecutive attempt(s) - next attempt in %lus.\n",
                (unsigned)g_wifiFailureStreak, g_wifiRetryDelayMs / 1000UL);
}
