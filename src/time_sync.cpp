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

// Same alert-once-per-transition/re-arm shape as g_nvsUsageAlerted/
// g_wifiRssiWeakAlerted above, for setupTime()'s own NTP sync failure -
// re-armed (set back false) the next time a sync actually succeeds, so a
// sustained outage (e.g. NTP blocked by a firewall) alerts once per
// startMonitoring()/reconnect stretch rather than on every single retry
// loop that calls setupTime() again.
static bool g_ntpSyncFailedAlerted = false;

// Called from setup(), before WiFi/NTP have had any chance to run - see
// rtc_store.h's own comment for why. No-op if the RTC isn't enabled/found
// (initRtc() must already have run). readRtcTime() gives back a UTC struct
// tm (this project's system clock is always UTC - see setupTime()'s own
// comment below), so timeGmUtc() (lib/rtc_ds3231 - a portable replacement
// for the standard timegm(), which this platform's libc doesn't provide;
// mktime() would be wrong here regardless, since it interprets its input
// as LOCAL time and the TZ env var hasn't even been set yet at this point
// in boot) is the correct conversion to an epoch value here.
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

// Last-resort fallback clock source, tried only when NTP has just failed
// AND there's no RTC to have already seeded a plausible time at boot (see
// setupTime()'s own call site). The problem this solves: Telegram's send
// path uses certificate-pinned TLS (WiFiClientSecure::setCACert, see
// telegram_ca.h's own comment on why this isn't setInsecure()), and
// mbedTLS's certificate validation checks the server certificate's
// NotBefore/NotAfter window against the CLIENT's current system time - a
// clock still sitting near the Unix epoch (1970) makes Telegram's real
// certificate look "not yet valid" and fails the handshake outright.
// Without this, setupTime()'s own NTP-failure alert right below would be
// silently unable to send in exactly the case it exists to warn about
// (no RTC + NTP down).
//
// Deliberately plain HTTP, not HTTPS, to the local network's own gateway -
// the whole point is a time source that doesn't itself need a working
// clock first. Every HTTP response carries a Date header (added by the
// server layer itself, RFC 7231), regardless of status code - a login
// page or a 401 challenge from the router's own admin UI still has one,
// so this doesn't need to authenticate or expect any particular response.
// Doesn't need to be accurate, just plausible - same bar
// seedSystemClockFromRtc() above already accepts; a real NTP sync later
// corrects it properly either way. Best-effort: Serial-only on any
// failure (no router reachable, no Date header, or one
// parseHttpDate/http_date_parse.h - IMF-fixdate only - doesn't recognize) -
// this is itself the last-resort fallback, so there's nothing further to
// fall back to.
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
  // Never follow a redirect here - some routers' "/" redirects straight to
  // their own HTTPS admin UI, which would just reintroduce the exact
  // TLS-needs-a-clock problem this function exists to avoid. The Date
  // header on the FIRST (redirect) response is just as valid as one from
  // a final 200 page.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  // Registered so http.header() can actually see it after GET() - without
  // collectHeaders(), HTTPClient doesn't expose an arbitrary header
  // through header() at all. See snapshot_fetch.cpp's fetchOneSnapshot for the
  // same requirement with Transfer-Encoding.
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
  // Must follow configTime() (does the actual esp_sntp_init()). No port
  // setting - ESP32's SNTP client hardcodes UDP port 123.
  //
  // Clamped here, at the point of use, not just at the dashboard save
  // (webserver_network.cpp's handleSaveNetwork clamps user input to
  // [1, NTP_SYNC_MAX_MINUTES] minutes) - same "hand-edited/imported NVS
  // blob bypasses the form entirely" reasoning already applied to
  // motionWatchdogHours (telegram_alerts.cpp's checkMotionWatchdog) and the SD
  // storage check interval below. Unlike those two, 0 isn't a legitimate
  // "disabled" sentinel here - esp_sntp_set_sync_interval(0) means resync
  // continuously, exactly the "hammer the NTP server" outcome
  // handleSaveNetwork's own comment says a blank/zero/negative field must
  // never produce; Import (config_backup.cpp's applyConfigImport)
  // writes this field with no clamp of its own at all.
  unsigned long safeSyncIntervalMs = g_wifiCredentials.ntpSyncIntervalMs;
  if (safeSyncIntervalMs == 0) safeSyncIntervalMs = 3600000UL; // 1h - same fallback network_store.cpp's own load-time default uses
  unsigned long maxSyncIntervalMs = NTP_SYNC_MAX_MINUTES * 60000UL;
  if (safeSyncIntervalMs > maxSyncIntervalMs) safeSyncIntervalMs = maxSyncIntervalMs;
  esp_sntp_set_sync_interval(safeSyncIntervalMs);

  // configTime() above set TZ to a no-op UTC form (gmtOffset=0/daylightOffset=0
  // - the system clock stays true UTC, see WifiCredentials::posixTz). This
  // overrides it with a real POSIX TZ rule if configured, affecting only
  // DST-aware local-time reads (telegram_transport.cpp's nowTimestampString) -
  // WS-Security's timestamp reads UTC directly via gmtime_r regardless.
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

      // Keeps the RTC corrected for the next boot - once per real sync,
      // not continuously. timeinfo above may be LOCAL time (posixTz-
      // adjusted) if a TZ is configured, so this reads UTC fresh via
      // gmtime_r rather than reusing it - the RTC always stores UTC (see
      // rtc_store.h's own comment).
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
  // Only worth trying if the RTC hasn't already given the clock a
  // plausible seed at boot (seedSystemClockFromRtc, before WiFi/NTP ever
  // ran) - see seedSystemClockFromRouterHttpDate's own comment for why
  // this specifically exists to give the alert below a chance to send.
  bool routerTimeSeeded = hasRtc ? false : seedSystemClockFromRouterHttpDate();
  logEvent(String("NTP sync failed") +
           (hasRtc ? " - using RTC time" : routerTimeSeeded ? " - using router's HTTP time" : " - no fallback time available"));
  if (!g_ntpSyncFailedAlerted) {
    g_ntpSyncFailedAlerted = true;
    bool hasFallbackTime = hasRtc || routerTimeSeeded;
    sendTelegramMessage([hasFallbackTime](TelegramLang lang) { return trNtpSyncFailed(lang, hasFallbackTime); });
  }
}
