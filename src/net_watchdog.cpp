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

// 1.1.1.1: a raw IP needs no DNS (which itself needs WAN), and it's
// independent of Telegram, so a Telegram outage doesn't look like no internet.
static const IPAddress kProbeAddr(1, 1, 1, 1);
static const uint16_t kProbePort = 443;
static const unsigned long kProbeTimeoutMs = 3000;

static bool g_settingEnabled = false;   // cached at boot, see initNetWatchdog()
static bool g_available = false;        // see netWatchdogActive()'s comment
static int g_activePin = -1;
static bool g_activeLow = true;
// Start of the current pulse interval (0 = no outage). Restarted after each
// pulse, so an ongoing outage is pulsed once per threshold. loop() only.
static unsigned long g_firstFailureMs = 0;

// When the current outage really began (0 = none). Not restarted by pulses, so
// "down since" stays accurate across pulse cycles.
static unsigned long g_outageEpisodeStartMs = 0;

NetWatchdogSettings loadNetWatchdogSettings() {
  Preferences prefs;
  // Read-write (see loadDashboardAuth).
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
                  "Hardware page, then reboot. Staying inactive.\n", settings.pin);
    return;
  }

  g_activePin = settings.pin;
  pinMode(g_activePin, OUTPUT);
  // Resting state: relay energized, router powered.
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

// Clamped at use; also keeps the blocking pulse under the 90s watchdog, which
// is reset right after.
static void pulseRelay(uint32_t pulseDurationMs) {
  uint32_t safePulseMs = pulseDurationMs;
  if (safePulseMs > NET_WATCHDOG_PULSE_MAX_MS) safePulseMs = NET_WATCHDOG_PULSE_MAX_MS;
  digitalWrite(g_activePin, g_activeLow ? HIGH : LOW); // de-energize - cuts router power
  delay(safePulseMs);
  digitalWrite(g_activePin, g_activeLow ? LOW : HIGH); // re-energize - restores power
  esp_task_wdt_reset();
}

bool netWatchdogManualPulse() {
  if (!netWatchdogActive()) return false;
  // A manual pulse leaves outage timing alone.
  pulseRelay(loadNetWatchdogSettings().pulseDurationMs);
  logEvent("Internet watchdog: manual test pulse");
  return true;
}

NetWatchdogCheckResult checkInternetAndMaybePulseRelay() {
  NetWatchdogCheckResult result;
  if (!netWatchdogActive()) return result;

  bool reachable = probeInternetReachable();
  unsigned long now = millis();

  if (reachable) {
    if (g_firstFailureMs != 0) {
      unsigned long startMs = g_outageEpisodeStartMs;
      logEvent("Internet watchdog: connectivity recovered");
      g_firstFailureMs = 0;
      g_outageEpisodeStartMs = 0;
      result.event = NetWatchdogCheckResult::Event::Recovered;
      result.outageStartMs = startMs;
      result.outageDurationMs = now - startMs;
    }
    return result;
  }

  if (g_firstFailureMs == 0) {
    g_firstFailureMs = now; // outage just started - nothing to do yet
    g_outageEpisodeStartMs = now; // true episode start - never touched again until recovery
    return result;
  }

  // Re-read during an outage so threshold/duration changes apply without a
  // reboot.
  NetWatchdogSettings settings = loadNetWatchdogSettings();
  uint32_t safeThresholdMs = settings.outageThresholdMs;
  if (safeThresholdMs > NET_WATCHDOG_THRESHOLD_MAX_MS) safeThresholdMs = NET_WATCHDOG_THRESHOLD_MAX_MS;

  if (!outageThresholdReached(g_firstFailureMs, now, safeThresholdMs)) return result;

  logEvent("Internet watchdog: outage threshold reached - pulsing relay");
  pulseRelay(settings.pulseDurationMs);
  // Restart rather than clear, so repeated pulses are spaced by the threshold.
  g_firstFailureMs = now;
  result.event = NetWatchdogCheckResult::Event::OutageDetected;
  return result;
}

NetWatchdogStatus getNetWatchdogStatus() {
  NetWatchdogStatus status;
  status.settingEnabled = g_settingEnabled;
  status.available = g_available;
  status.outageInProgress = g_firstFailureMs != 0;
  status.outageStartMs = g_outageEpisodeStartMs;
  return status;
}
