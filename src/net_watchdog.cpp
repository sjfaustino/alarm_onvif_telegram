#include "net_watchdog.h"
#include "net_watchdog_logic.h"
#include "event_log_store.h" // logEvent
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>

static const char* NVS_NAMESPACE = "netwatchdog";
static const char* NVS_KEY_ENABLED = "enabled";
static const char* NVS_KEY_PIN = "pin";
static const char* NVS_KEY_ACTIVE_LOW = "activeLow";
static const char* NVS_KEY_THRESHOLD_MS = "threshMs";
static const char* NVS_KEY_PULSE_MS = "pulseMs";

// Cloudflare's anycast DNS resolver - fixed, not dashboard-configurable
// (see net_watchdog.h's own comment on why). A raw IP needs no DNS lookup
// of its own (DNS itself needs working WAN to resolve anything, so a
// hostname-based probe would risk conflating "DNS is broken" with "no
// internet"), and has very high uptime independent of this project's
// other network dependency (api.telegram.org) - a probe target that only
// failed because Telegram itself was down would otherwise misattribute a
// Telegram-side outage as "no internet."
static const IPAddress kProbeAddr(1, 1, 1, 1);
static const uint16_t kProbePort = 443;
static const unsigned long kProbeTimeoutMs = 3000;

static bool g_settingEnabled = false;   // cached at boot, see initNetWatchdog()
static bool g_available = false;        // see netWatchdogActive()'s comment
static int g_activePin = -1;
static bool g_activeLow = true;
// 0 = no outage currently in progress - a millis() timestamp of when the
// CURRENT unbroken stretch of failed probes started, same "0 is the
// sentinel for none scheduled" convention CameraState::scheduledRevertDueMs
// already uses. Same-task-only (main.cpp's loop() is the only caller of
// checkInternetAndMaybePulseRelay/getNetWatchdogStatus), no lock needed.
static unsigned long g_firstFailureMs = 0;

NetWatchdogSettings loadNetWatchdogSettings() {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(NVS_NAMESPACE, false);
  NetWatchdogSettings s;
  s.enabled = prefs.getBool(NVS_KEY_ENABLED, false);
  s.pin = prefs.getInt(NVS_KEY_PIN, NET_WATCHDOG_PIN_DEFAULT);
  s.activeLow = prefs.getBool(NVS_KEY_ACTIVE_LOW, true);
  s.outageThresholdMs = prefs.getUInt(NVS_KEY_THRESHOLD_MS, s.outageThresholdMs);
  s.pulseDurationMs = prefs.getUInt(NVS_KEY_PULSE_MS, s.pulseDurationMs);
  prefs.end();
  return s;
}

bool saveNetWatchdogSettings(const NetWatchdogSettings& settings) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = prefs.putBool(NVS_KEY_ENABLED, settings.enabled) > 0 &&
            prefs.putInt(NVS_KEY_PIN, settings.pin) > 0 &&
            prefs.putBool(NVS_KEY_ACTIVE_LOW, settings.activeLow) > 0 &&
            prefs.putUInt(NVS_KEY_THRESHOLD_MS, settings.outageThresholdMs) > 0 &&
            prefs.putUInt(NVS_KEY_PULSE_MS, settings.pulseDurationMs) > 0;
  prefs.end();
  if (!ok) {
    Serial.println("[net_watchdog] ERROR: failed to persist settings to NVS - they will revert to "
                    "the previous values on the next reboot.");
  }
  return ok;
}

void initNetWatchdog() {
  NetWatchdogSettings settings = loadNetWatchdogSettings();
  g_settingEnabled = settings.enabled;
  g_activeLow = settings.activeLow;

  if (!g_settingEnabled) {
    Serial.println("[net_watchdog] Internet watchdog is disabled.");
    return;
  }

  if (isReservedOrUnsafePin(settings.pin)) {
    Serial.printf("[net_watchdog] Internet watchdog is enabled, but pin %d is reserved by another "
                  "peripheral or unsafe to use on this board - pick a different pin on the "
                  "Maintenance page, then reboot. Staying inactive.\n", settings.pin);
    return;
  }

  g_activePin = settings.pin;
  pinMode(g_activePin, OUTPUT);
  // Resting state - relay energized, router powered normally. A pulse
  // later temporarily drives the opposite level, then returns here.
  digitalWrite(g_activePin, g_activeLow ? LOW : HIGH);
  g_available = true;
  Serial.printf("[net_watchdog] Internet watchdog active on pin %d.\n", g_activePin);
}

bool netWatchdogActive() {
  return g_settingEnabled && g_available;
}

static bool probeInternetReachable() {
  WiFiClient client;
  client.setTimeout(kProbeTimeoutMs);
  bool ok = client.connect(kProbeAddr, kProbePort);
  if (ok) client.stop();
  return ok;
}

// Clamped here, at the point of use, not just at the dashboard form
// boundary - same "hand-edited/imported NVS blob bypasses the form
// entirely" reasoning as every other clamp in this project. The pulse
// delay runs on loop()'s own task (the only one subscribed to the task
// watchdog), so the ceiling also keeps it comfortably under the 90s TWDT
// timeout - esp_task_wdt_reset() right after covers the pulse itself,
// same single-reset-after-a-bounded-blocking-op pattern checkWifiSignal's
// own Telegram send already uses (not the per-file reset loop the
// *unbounded* SD retention sweep needs).
static void pulseRelay(uint32_t pulseDurationMs) {
  uint32_t safePulseMs = pulseDurationMs;
  if (safePulseMs > NET_WATCHDOG_PULSE_MAX_MS) safePulseMs = NET_WATCHDOG_PULSE_MAX_MS;
  digitalWrite(g_activePin, g_activeLow ? HIGH : LOW); // de-energize - cuts router power
  delay(safePulseMs);
  digitalWrite(g_activePin, g_activeLow ? LOW : HIGH); // re-energize - restores power
  esp_task_wdt_reset();
}

bool checkInternetAndMaybePulseRelay() {
  if (!netWatchdogActive()) return false;

  bool reachable = probeInternetReachable();
  unsigned long now = millis();

  if (reachable) {
    if (g_firstFailureMs != 0) {
      logEvent("Internet watchdog: connectivity recovered");
      g_firstFailureMs = 0;
    }
    return false;
  }

  if (g_firstFailureMs == 0) {
    g_firstFailureMs = now; // outage just started - nothing to do yet
    return false;
  }

  // Re-read settings only while an outage is actually in progress (rare) -
  // lets the threshold/pulse-duration dials take effect without a reboot,
  // unlike enabled/pin which need pinMode() freshly applied at boot.
  NetWatchdogSettings settings = loadNetWatchdogSettings();
  uint32_t safeThresholdMs = settings.outageThresholdMs;
  if (safeThresholdMs > NET_WATCHDOG_THRESHOLD_MAX_MS) safeThresholdMs = NET_WATCHDOG_THRESHOLD_MAX_MS;

  if (!outageThresholdReached(g_firstFailureMs, now, safeThresholdMs)) return false;

  logEvent("Internet watchdog: outage threshold reached - pulsing relay");
  pulseRelay(settings.pulseDurationMs);
  // Restart the timer rather than clearing it - naturally spaces repeated
  // pulses by the same configured threshold if the outage outlives one
  // power-cycle attempt (the router needs real time to reboot and
  // reacquire 4G), without a separate backoff setting.
  g_firstFailureMs = now;
  return true;
}

NetWatchdogStatus getNetWatchdogStatus() {
  NetWatchdogStatus status;
  status.settingEnabled = g_settingEnabled;
  status.available = g_available;
  status.outageInProgress = g_firstFailureMs != 0;
  status.outageStartMs = g_firstFailureMs;
  return status;
}
