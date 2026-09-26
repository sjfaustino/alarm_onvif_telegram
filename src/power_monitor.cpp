#include "power_monitor.h"
#include "net_watchdog.h" // loadNetWatchdogSettings - watchdogPinsConflict cross-check
#include "bridge_watchdog.h" // loadBridgeWatchdogSettings - watchdogPinsConflict cross-check
#include "net_watchdog_logic.h" // isReservedOrUnsafePin, watchdogPinsConflict, outageThresholdReached
#include "event_log_store.h" // logEvent
#include <Preferences.h>

static const char* NVS_NAMESPACE = "powermon";
static const char* NVS_KEY_ENABLED = "enabled";
static const char* NVS_KEY_PIN = "pin";
static const char* NVS_KEY_ACTIVE_HIGH = "activeHigh";

static bool g_settingEnabled = false;   // cached at boot, see initPowerMonitor()
static bool g_available = false;        // see powerMonitorActive()'s comment
static int g_activePin = -1;
static bool g_activeHigh = false;
static bool g_confirmedPowerPresent = true; // last CONFIRMED (debounced) reading
// When a disagreeing reading was first seen (0 = none). loop() only.
static unsigned long g_pendingSinceMs = 0;

PowerMonitorSettings loadPowerMonitorSettings() {
  Preferences prefs;
  // Read-write (see loadDashboardAuth).
  prefs.begin(NVS_NAMESPACE, false);
  PowerMonitorSettings s;
  s.enabled = prefs.getBool(NVS_KEY_ENABLED, false);
  s.pin = prefs.getInt(NVS_KEY_PIN, POWER_MONITOR_PIN_DEFAULT);
  s.activeHigh = prefs.getBool(NVS_KEY_ACTIVE_HIGH, false);
  prefs.end();
  return s;
}

bool savePowerMonitorSettings(const PowerMonitorSettings& settings) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = prefs.putBool(NVS_KEY_ENABLED, settings.enabled) > 0 &&
            prefs.putInt(NVS_KEY_PIN, settings.pin) > 0 &&
            prefs.putBool(NVS_KEY_ACTIVE_HIGH, settings.activeHigh) > 0;
  prefs.end();
  if (!ok) {
    Serial.println("[power_monitor] ERROR: failed to persist settings to NVS - they will revert to "
                    "the previous values on the next reboot.");
  }
  return ok;
}

static bool readConfirmedFromPin() {
  bool raw = digitalRead(g_activePin) == HIGH;
  return raw == g_activeHigh;
}

void initPowerMonitor() {
  PowerMonitorSettings settings = loadPowerMonitorSettings();
  g_settingEnabled = settings.enabled;
  g_activeHigh = settings.activeHigh;

  if (!g_settingEnabled) {
    Serial.println("[power_monitor] 220V power monitor is disabled.");
    return;
  }

  if (isReservedOrUnsafePin(settings.pin)) {
    Serial.printf("[power_monitor] 220V power monitor is enabled, but pin %d is reserved by another "
                  "peripheral or unsafe to use on this board - pick a different pin on the Hardware "
                  "page, then reboot. Staying inactive.\n", settings.pin);
    return;
  }

  // Backstop for imported configs; the save routes are the main pin-clash
  // guard.
  NetWatchdogSettings netSettings = loadNetWatchdogSettings();
  BridgeWatchdogSettings bridgeSettings = loadBridgeWatchdogSettings();
  if (watchdogPinsConflict(true, settings.pin, netSettings.enabled, netSettings.pin)) {
    Serial.printf("[power_monitor] 220V power monitor is enabled on pin %d, but the Internet Watchdog "
                  "is also configured for that pin - pick a different pin for one of them on the "
                  "Hardware page, then reboot. Staying inactive.\n", settings.pin);
    return;
  }
  if (watchdogPinsConflict(true, settings.pin, bridgeSettings.enabled, bridgeSettings.pin)) {
    Serial.printf("[power_monitor] 220V power monitor is enabled on pin %d, but the Camera Bridge "
                  "Watchdog is also configured for that pin - pick a different pin for one of them on "
                  "the Hardware page, then reboot. Staying inactive.\n", settings.pin);
    return;
  }

  g_activePin = settings.pin;
  // Pull toward the open-contact level: with COM->3.3V, a pull-up would read
  // HIGH whether open or closed, and outages would never be seen.
  pinMode(g_activePin, settings.activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  g_confirmedPowerPresent = readConfirmedFromPin();
  g_pendingSinceMs = 0;
  g_available = true;
  Serial.printf("[power_monitor] 220V power monitor active on pin %d - mains power currently %s.\n",
                g_activePin, g_confirmedPowerPresent ? "ON" : "OFF");
}

bool powerMonitorActive() {
  return g_settingEnabled && g_available;
}

bool checkPowerStateChanged() {
  if (!powerMonitorActive()) return false;

  bool rawPresent = readConfirmedFromPin();
  unsigned long now = millis();

  if (rawPresent == g_confirmedPowerPresent) {
    g_pendingSinceMs = 0; // agrees with the confirmed state - nothing pending
    return false;
  }

  if (g_pendingSinceMs == 0) {
    g_pendingSinceMs = now; // just started disagreeing - wait to confirm
    return false;
  }

  // Generic "enough time passed" check despite the name.
  if (!outageThresholdReached(g_pendingSinceMs, now, POWER_MONITOR_DEBOUNCE_MS)) return false;

  g_confirmedPowerPresent = rawPresent;
  g_pendingSinceMs = 0;
  logEvent(String("Mains power ") + (g_confirmedPowerPresent ? "restored" : "lost"));
  return true;
}

PowerMonitorStatus getPowerMonitorStatus() {
  PowerMonitorStatus status;
  status.settingEnabled = g_settingEnabled;
  status.available = g_available;
  status.powerPresent = g_confirmedPowerPresent;
  if (status.available) status.rawPinHigh = digitalRead(g_activePin) == HIGH;
  return status;
}
