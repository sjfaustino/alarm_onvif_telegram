#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h> // seedSystemClockFromRouterHttpDate
#include <esp_sntp.h>
#include <cstdlib>
#include <time.h>      // gmtime_r - setupTime's RTC writeback
#include <sys/time.h>  // settimeofday
#include "time_sync.h"
#include "wifi_connect.h" // g_wifiCredentials
#include "config.h"
#include "event_log_store.h"
#include "rtc_store.h"
#include "rtc_ds3231.h"      // timeGmUtc
#include "http_date_parse.h" // parseHttpDate - seedSystemClockFromRouterHttpDate
#include "telegram.h"
#include "telegram_i18n.h"

// Alert once per NTP outage; re-armed by the next successful sync.
static bool g_ntpSyncFailedAlerted = false;

// Before WiFi. The RTC stores UTC, and timeGmUtc replaces the missing timegm()
// (mktime would apply a TZ that isn't set yet anyway).
void seedSystemClockFromRtc() {
  if (!rtcActive()) return;

  struct tm rtcTime;
  if (!readRtcTime(&rtcTime)) {
    Serial.println("[rtc_store] RTC found but reading its time failed - system clock not seeded from it.");
    return;
  }

  struct timeval tv = { timeGmUtc(rtcTime), 0 };
  settimeofday(&tv, nullptr);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &rtcTime);
  Serial.printf("[rtc_store] Seeded system clock from RTC: %s UTC (NTP will refine this once WiFi connects)\n", buf);
}

// Last-resort clock when NTP failed and there's no RTC. Telegram's pinned TLS
// rejects its certificate when the clock is near 1970, so without this the
// NTP-failure alert couldn't be sent at all.
//
// Plain HTTP to the gateway, which needs no clock. Any response (even a login
// page or 401) carries a Date header. It only has to be plausible; NTP
// corrects it later.
static bool seedSystemClockFromRouterHttpDate() {
  IPAddress gateway = WiFi.gatewayIP();
  if (gateway == IPAddress(0, 0, 0, 0)) {
    Serial.println("[setupTime] No known gateway IP - can't ask the router for the time.");
    return false;
  }

  HTTPClient http;
  if (!http.begin("http://" + gateway.toString() + "/")) {
    Serial.println("[setupTime] Could not start an HTTP request to the router.");
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  // Don't follow redirects: they often lead to the router's HTTPS UI, which
  // needs a clock. The redirect's own Date header works.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  // HTTPClient only exposes headers registered with collectHeaders().
  static const char* kDateHeader[] = {"Date"};
  http.collectHeaders(kDateHeader, 1);

  int code = http.GET();
  if (code <= 0) {
    Serial.printf("[setupTime] Router at %s didn't answer an HTTP request (%s).\n",
                  gateway.toString().c_str(), HTTPClient::errorToString(code).c_str());
    http.end();
    return false;
  }

  String dateHeader = http.header("Date");
  http.end();
  if (dateHeader.length() == 0) {
    Serial.println("[setupTime] Router's HTTP response had no Date header.");
    return false;
  }

  struct tm parsed;
  if (!parseHttpDate(dateHeader, parsed)) {
    Serial.printf("[setupTime] Router's Date header (\"%s\") didn't match the expected format.\n",
                  dateHeader.c_str());
    return false;
  }

  struct timeval tv = { timeGmUtc(parsed), 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("[setupTime] Seeded system clock from router's HTTP Date header: %s\n", dateHeader.c_str());
  return true;
}

void setupTime() {
  Serial.printf("Synchronizing UTC time from %s...\n", g_wifiCredentials.ntpServer.c_str());
  configTime(0, 0, g_wifiCredentials.ntpServer.c_str());
  // Must follow configTime(). Clamped at use: imported configs bypass the
  // form, and an interval of 0 means resync continuously.
  unsigned long safeSyncIntervalMs = g_wifiCredentials.ntpSyncIntervalMs;
  if (safeSyncIntervalMs == 0) safeSyncIntervalMs = 3600000UL; // 1h - same fallback network_store.cpp's own load-time default uses
  unsigned long maxSyncIntervalMs = NTP_SYNC_MAX_MINUTES * 60000UL;
  if (safeSyncIntervalMs > maxSyncIntervalMs) safeSyncIntervalMs = maxSyncIntervalMs;
  esp_sntp_set_sync_interval(safeSyncIntervalMs);

  // The system clock stays UTC; TZ only affects local-time display.
  if (g_wifiCredentials.posixTz.length() > 0) {
    setenv("TZ", g_wifiCredentials.posixTz.c_str(), 1);
    tzset();
    Serial.printf("Local timezone for alert captions: %s\n", g_wifiCredentials.posixTz.c_str());
  }

  struct tm timeinfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo, 1000)) {
      char buf[25];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      Serial.printf("NTP time synchronized: %s\n", buf);

      // Write UTC to the RTC (timeinfo may be local time).
      if (rtcActive()) {
        time_t now;
        time(&now);
        struct tm utcTime;
        gmtime_r(&now, &utcTime);
        if (writeRtcTime(utcTime)) {
          Serial.println("[rtc_store] RTC corrected from this NTP sync.");
        } else {
          Serial.println("[rtc_store] WARNING: failed to write corrected time to the RTC.");
        }
      }
      g_ntpSyncFailedAlerted = false; // re-arm - see its own comment
      return;
    }
    Serial.print(".");
  }
  Serial.println("\nWARNING: NTP synchronization failed.");
  bool hasRtc = rtcActive();
  // Only without an RTC, which already seeded a plausible time.
  bool routerTimeSeeded = hasRtc ? false : seedSystemClockFromRouterHttpDate();
  logEvent(String("NTP sync failed") +
           (hasRtc ? " - using RTC time" : routerTimeSeeded ? " - using router's HTTP time" : " - no fallback time available"));
  if (!g_ntpSyncFailedAlerted) {
    g_ntpSyncFailedAlerted = true;
    bool hasFallbackTime = hasRtc || routerTimeSeeded;
    sendTelegramMessage([hasFallbackTime](TelegramLang lang) { return trNtpSyncFailed(lang, hasFallbackTime); });
  }
}
