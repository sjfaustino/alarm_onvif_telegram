#include "bridge_watchdog.h"
#include "net_watchdog.h" // loadNetWatchdogSettings - watchdogPinsConflict cross-check
#include "net_watchdog_logic.h"
#include "event_log_store.h" // logEvent
#include <Preferences.h>
#include <esp_task_wdt.h>

static const char* NVS_NAMESPACE = "bridgewd";
static const char* NVS_KEY_ENABLED = "enabled";
static const char* NVS_KEY_CAMERA_A = "camA";
static const char* NVS_KEY_CAMERA_B = "camB";
static const char* NVS_KEY_PIN = "pin";
static const char* NVS_KEY_ACTIVE_LOW = "activeLow";
static const char* NVS_KEY_THRESHOLD_MS = "threshMs";
static const char* NVS_KEY_PULSE_MS = "pulseMs";

static bool g_settingEnabled = false;   // cached at boot, see initBridgeWatchdog()
static bool g_available = false;        // see bridgeWatchdogActive()'s comment
static int g_activePin = -1;
static bool g_activeLow = true;
static bool g_camerasResolved = false;  // updated on each check - see checkBridgeCamerasAndMaybePulseRelay
// 0 = no outage currently in progress - a millis() timestamp of when the
// CURRENT unbroken stretch of "both offline" started, same sentinel
// convention net_watchdog.cpp's own g_firstFailureMs uses. Same-task-only
// (main.cpp's loop() is the only caller here), no lock needed.
static unsigned long g_firstBothOfflineMs = 0;

BridgeWatchdogSettings loadBridgeWatchdogSettings() {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(NVS_NAMESPACE, false);
  BridgeWatchdogSettings s;
  s.enabled = prefs.getBool(NVS_KEY_ENABLED, false);
  s.cameraA = prefs.getString(NVS_KEY_CAMERA_A, "");
  s.cameraB = prefs.getString(NVS_KEY_CAMERA_B, "");
  s.pin = prefs.getInt(NVS_KEY_PIN, BRIDGE_WATCHDOG_PIN_DEFAULT);
  s.activeLow = prefs.getBool(NVS_KEY_ACTIVE_LOW, true);
  s.outageThresholdMs = prefs.getUInt(NVS_KEY_THRESHOLD_MS, s.outageThresholdMs);
  s.pulseDurationMs = prefs.getUInt(NVS_KEY_PULSE_MS, s.pulseDurationMs);
  prefs.end();
  return s;
}

bool saveBridgeWatchdogSettings(const BridgeWatchdogSettings& settings) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = prefs.putBool(NVS_KEY_ENABLED, settings.enabled) > 0 &&
            prefs.putString(NVS_KEY_CAMERA_A, settings.cameraA) > 0 &&
            prefs.putString(NVS_KEY_CAMERA_B, settings.cameraB) > 0 &&
            prefs.putInt(NVS_KEY_PIN, settings.pin) > 0 &&
            prefs.putBool(NVS_KEY_ACTIVE_LOW, settings.activeLow) > 0 &&
            prefs.putUInt(NVS_KEY_THRESHOLD_MS, settings.outageThresholdMs) > 0 &&
            prefs.putUInt(NVS_KEY_PULSE_MS, settings.pulseDurationMs) > 0;
  prefs.end();
  if (!ok) {
    Serial.println("[bridge_watchdog] ERROR: failed to persist settings to NVS - they will revert to "
                    "the previous values on the next reboot.");
  }
  return ok;
}

void initBridgeWatchdog() {
  BridgeWatchdogSettings settings = loadBridgeWatchdogSettings();
  g_settingEnabled = settings.enabled;
  g_activeLow = settings.activeLow;

  if (!g_settingEnabled) {
    Serial.println("[bridge_watchdog] Camera bridge watchdog is disabled.");
    return;
  }

  if (isReservedOrUnsafePin(settings.pin)) {
    Serial.printf("[bridge_watchdog] Camera bridge watchdog is enabled, but pin %d is reserved by "
                  "another peripheral or unsafe to use on this board - pick a different pin on the "
                  "Hardware page, then reboot. Staying inactive.\n", settings.pin);
    return;
  }

  // Defense in depth - the dashboard save routes (webserver.cpp) are the
  // primary guard against both watchdogs sharing a pin; this catches a
  // hand-edited/imported NVS record that bypassed them.
  NetWatchdogSettings netSettings = loadNetWatchdogSettings();
  if (watchdogPinsConflict(true, settings.pin, netSettings.enabled, netSettings.pin)) {
    Serial.printf("[bridge_watchdog] Camera bridge watchdog is enabled on pin %d, but the Internet "
                  "Watchdog is also configured for that pin - pick a different pin for one of them on "
                  "the Hardware page, then reboot. Staying inactive.\n", settings.pin);
    return;
  }

  g_activePin = settings.pin;
  pinMode(g_activePin, OUTPUT);
  // Resting state - relay energized, bridge powered normally. A pulse
  // later temporarily drives the opposite level, then returns here.
  digitalWrite(g_activePin, g_activeLow ? LOW : HIGH);
  g_available = true;
  Serial.printf("[bridge_watchdog] Camera bridge watchdog active on pin %d.\n", g_activePin);
}

bool bridgeWatchdogActive() {
  return g_settingEnabled && g_available;
}

// Case-insensitive match, same as webserver.cpp's own camera-by-name
// lookup (the /cameras/snapshot route).
static int findCameraIndexByName(const CameraConfig cameras[], size_t numCameras, const String& name) {
  if (name.length() == 0) return -1;
  for (size_t i = 0; i < numCameras; i++) {
    if (cameras[i].name.equalsIgnoreCase(name)) return (int)i;
  }
  return -1;
}

// Clamped here, at the point of use, not just at the dashboard form
// boundary - same "hand-edited/imported NVS blob bypasses the form
// entirely" reasoning as every other clamp in this project. The pulse
// delay runs on loop()'s own task (the only one subscribed to the task
// watchdog), so the ceiling also keeps it comfortably under the 90s TWDT
// timeout - esp_task_wdt_reset() right after covers the pulse itself,
// same pattern net_watchdog.cpp's own pulseRelay uses.
static void pulseRelay(uint32_t pulseDurationMs) {
  uint32_t safePulseMs = pulseDurationMs;
  if (safePulseMs > BRIDGE_WATCHDOG_PULSE_MAX_MS) safePulseMs = BRIDGE_WATCHDOG_PULSE_MAX_MS;
  digitalWrite(g_activePin, g_activeLow ? HIGH : LOW); // de-energize - cuts bridge power
  delay(safePulseMs);
  digitalWrite(g_activePin, g_activeLow ? LOW : HIGH); // re-energize - restores power
  esp_task_wdt_reset();
}

BridgeWatchdogCheckResult checkBridgeCamerasAndMaybePulseRelay(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  BridgeWatchdogCheckResult result;
  if (!bridgeWatchdogActive()) return result;

  BridgeWatchdogSettings settings = loadBridgeWatchdogSettings();
  int idxA = findCameraIndexByName(cameras, numCameras, settings.cameraA);
  int idxB = findCameraIndexByName(cameras, numCameras, settings.cameraB);

  // Fail-safe: a renamed/deleted camera (or a name that never matched)
  // means the pair can't be evaluated at all - never guess "both down" or
  // "both up" from incomplete information. Doesn't touch the timer, so a
  // transient mismatch mid-edit doesn't lose progress toward an
  // already-in-progress outage.
  if (idxA < 0 || idxB < 0 || !cameras[idxA].enabled || !cameras[idxB].enabled) {
    g_camerasResolved = false;
    return result;
  }
  g_camerasResolved = true;

  bool offlineA, offlineB;
  {
    CameraStateLock lockA(states[idxA]);
    offlineA = states[idxA].isOffline;
  }
  {
    CameraStateLock lockB(states[idxB]);
    offlineB = states[idxB].isOffline;
  }
  bool bothOffline = offlineA && offlineB;
  unsigned long now = millis();

  if (!bothOffline) {
    if (g_firstBothOfflineMs != 0) {
      unsigned long startMs = g_firstBothOfflineMs;
      logEvent("Camera bridge watchdog: " + settings.cameraA + " / " + settings.cameraB + " recovered");
      g_firstBothOfflineMs = 0;
      result.event = BridgeWatchdogCheckResult::Event::Recovered;
      result.outageStartMs = startMs;
      result.outageDurationMs = now - startMs;
    }
    return result;
  }

  if (g_firstBothOfflineMs == 0) {
    g_firstBothOfflineMs = now; // outage just started - nothing to do yet
    return result;
  }

  // Re-read settings only while an outage is actually in progress (rare) -
  // lets the threshold/pulse-duration dials take effect without a reboot,
  // unlike enabled/pin which need pinMode() freshly applied at boot. Reuse
  // the settings already loaded above rather than reloading again.
  uint32_t safeThresholdMs = settings.outageThresholdMs;
  if (safeThresholdMs > BRIDGE_WATCHDOG_THRESHOLD_MAX_MS) safeThresholdMs = BRIDGE_WATCHDOG_THRESHOLD_MAX_MS;

  if (!outageThresholdReached(g_firstBothOfflineMs, now, safeThresholdMs)) return result;

  logEvent("Camera bridge watchdog: " + settings.cameraA + " / " + settings.cameraB +
           " both offline past threshold - pulsing relay");
  pulseRelay(settings.pulseDurationMs);
  // Restart the timer rather than clearing it - naturally spaces repeated
  // pulses by the same configured threshold if the outage outlives one
  // power-cycle attempt, without a separate backoff setting - same
  // reasoning as net_watchdog.cpp's own g_firstFailureMs restart.
  g_firstBothOfflineMs = now;
  result.event = BridgeWatchdogCheckResult::Event::OutageDetected;
  return result;
}

BridgeWatchdogStatus getBridgeWatchdogStatus() {
  BridgeWatchdogStatus status;
  status.settingEnabled = g_settingEnabled;
  status.available = g_available;
  status.camerasResolved = g_camerasResolved;
  status.outageInProgress = g_firstBothOfflineMs != 0;
  status.outageStartMs = g_firstBothOfflineMs;
  return status;
}
