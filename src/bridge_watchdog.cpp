#include "bridge_watchdog.h"
#include "camera.h" // CameraState - only forward-declared in bridge_watchdog.h, see its own comment
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
// Start of the current pulse interval (0 = none); restarted after each pulse,
// as in net_watchdog.cpp. loop() only.
static unsigned long g_firstBothOfflineMs = 0;

// When the current outage really began; not restarted by pulses.
static unsigned long g_outageEpisodeStartMs = 0;

BridgeWatchdogSettings loadBridgeWatchdogSettings() {
  Preferences prefs;
  // Read-write (see loadDashboardAuth).
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

  // Backstop for imported configs; the save routes are the main pin-clash
  // guard.
  NetWatchdogSettings netSettings = loadNetWatchdogSettings();
  if (watchdogPinsConflict(true, settings.pin, netSettings.enabled, netSettings.pin)) {
    Serial.printf("[bridge_watchdog] Camera bridge watchdog is enabled on pin %d, but the Internet "
                  "Watchdog is also configured for that pin - pick a different pin for one of them on "
                  "the Hardware page, then reboot. Staying inactive.\n", settings.pin);
    return;
  }

  g_activePin = settings.pin;
  pinMode(g_activePin, OUTPUT);
  // Resting state: relay energized, bridge powered.
  digitalWrite(g_activePin, g_activeLow ? LOW : HIGH);
  g_available = true;
  Serial.printf("[bridge_watchdog] Camera bridge watchdog active on pin %d.\n", g_activePin);
}

bool bridgeWatchdogActive() {
  return g_settingEnabled && g_available;
}

static int findCameraIndexByName(const CameraConfig cameras[], size_t numCameras, const String& name) {
  if (name.length() == 0) return -1;
  for (size_t i = 0; i < numCameras; i++) {
    if (cameras[i].name.equalsIgnoreCase(name)) return (int)i;
  }
  return -1;
}

// Clamped at use; keeps the blocking pulse under the 90s watchdog.
static void pulseRelay(uint32_t pulseDurationMs) {
  uint32_t safePulseMs = pulseDurationMs;
  if (safePulseMs > BRIDGE_WATCHDOG_PULSE_MAX_MS) safePulseMs = BRIDGE_WATCHDOG_PULSE_MAX_MS;
  digitalWrite(g_activePin, g_activeLow ? HIGH : LOW); // de-energize - cuts bridge power
  delay(safePulseMs);
  digitalWrite(g_activePin, g_activeLow ? LOW : HIGH); // re-energize - restores power
  esp_task_wdt_reset();
}

bool bridgeWatchdogManualPulse() {
  if (!bridgeWatchdogActive()) return false;
  // A manual pulse leaves outage timing alone.
  pulseRelay(loadBridgeWatchdogSettings().pulseDurationMs);
  logEvent("Camera bridge watchdog: manual test pulse");
  return true;
}

BridgeWatchdogCheckResult checkBridgeCamerasAndMaybePulseRelay(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  BridgeWatchdogCheckResult result;
  if (!bridgeWatchdogActive()) return result;

  BridgeWatchdogSettings settings = loadBridgeWatchdogSettings();
  int idxA = findCameraIndexByName(cameras, numCameras, settings.cameraA);
  int idxB = findCameraIndexByName(cameras, numCameras, settings.cameraB);

  // Can't evaluate the pair (renamed, deleted, disabled): do nothing and keep
  // the timer, rather than guess.
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
      unsigned long startMs = g_outageEpisodeStartMs;
      logEvent("Camera bridge watchdog: " + settings.cameraA + " / " + settings.cameraB + " recovered");
      g_firstBothOfflineMs = 0;
      g_outageEpisodeStartMs = 0;
      result.event = BridgeWatchdogCheckResult::Event::Recovered;
      result.outageStartMs = startMs;
      result.outageDurationMs = now - startMs;
    }
    return result;
  }

  if (g_firstBothOfflineMs == 0) {
    g_firstBothOfflineMs = now; // outage just started - nothing to do yet
    g_outageEpisodeStartMs = now; // true episode start - never touched again until recovery
    return result;
  }

  // Re-read during an outage so setting changes apply without a reboot.
  uint32_t safeThresholdMs = settings.outageThresholdMs;
  if (safeThresholdMs > BRIDGE_WATCHDOG_THRESHOLD_MAX_MS) safeThresholdMs = BRIDGE_WATCHDOG_THRESHOLD_MAX_MS;

  if (!outageThresholdReached(g_firstBothOfflineMs, now, safeThresholdMs)) return result;

  logEvent("Camera bridge watchdog: " + settings.cameraA + " / " + settings.cameraB +
           " both offline past threshold - pulsing relay");
  pulseRelay(settings.pulseDurationMs);
  // Restart rather than clear, spacing repeated pulses by the threshold.
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
  status.outageStartMs = g_outageEpisodeStartMs;
  return status;
}
