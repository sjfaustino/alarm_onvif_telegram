#include "hardware_form.h"
#include "config.h"
#include "net_watchdog_logic.h" // isReservedOrUnsafePin, watchdogPinsConflict

// Clamped in entered units before scaling: the multiply overflows 32 bits on a
// huge entry, which would wrap to a tiny outage threshold or pulse. Re-clamped
// at use in net_watchdog.cpp and bridge_watchdog.cpp.
static uint32_t parseScaledMs(const FormParams& params, const char* name, const char* fallback, unsigned long unitMs,
                              uint32_t maxMs) {
  long entered = params.get(name, fallback).toInt();
  long maxEntered = (long)(maxMs / unitMs);
  if (entered < 1) entered = 1;
  if (entered > maxEntered) entered = maxEntered;
  return (uint32_t)((unsigned long)entered * unitMs);
}

NetWatchdogSettings parseNetWatchdogForm(const FormParams& params) {
  NetWatchdogSettings s;
  s.enabled = params.has("enabled");
  s.activeLow = params.has("activeLow");
  s.pin = params.get("pin", String(NET_WATCHDOG_PIN_DEFAULT).c_str()).toInt();
  s.outageThresholdMs =
      parseScaledMs(params, "outageThresholdMinutes", "5", 60000UL, NET_WATCHDOG_THRESHOLD_MAX_MS);
  s.pulseDurationMs = parseScaledMs(params, "pulseSeconds", "10", 1000UL, NET_WATCHDOG_PULSE_MAX_MS);
  return s;
}

BridgeWatchdogSettings parseBridgeWatchdogForm(const FormParams& params) {
  BridgeWatchdogSettings s;
  s.enabled = params.has("enabled");
  s.activeLow = params.has("activeLow");
  s.cameraA = params.get("cameraA", "");
  s.cameraB = params.get("cameraB", "");
  s.pin = params.get("pin", String(BRIDGE_WATCHDOG_PIN_DEFAULT).c_str()).toInt();
  s.outageThresholdMs =
      parseScaledMs(params, "outageThresholdMinutes", "5", 60000UL, BRIDGE_WATCHDOG_THRESHOLD_MAX_MS);
  s.pulseDurationMs = parseScaledMs(params, "pulseSeconds", "10", 1000UL, BRIDGE_WATCHDOG_PULSE_MAX_MS);
  return s;
}

PowerMonitorSettings parsePowerMonitorForm(const FormParams& params) {
  PowerMonitorSettings s;
  s.enabled = params.has("enabled");
  s.activeHigh = params.has("activeHigh");
  s.pin = params.get("pin", String(POWER_MONITOR_PIN_DEFAULT).c_str()).toInt();
  return s;
}

static String reservedPinError(int pin) {
  return "Pin " + String(pin) + " is reserved by another peripheral on this board or "
         "unsafe to use for general GPIO - pick a different one. Not saved.";
}

static String pinClashError(int pin, const char* otherFeature) {
  return "Pin " + String(pin) + " is already used by the " + otherFeature + " - pick a different one. Not saved.";
}

String validateNetWatchdogSettings(const NetWatchdogSettings& s, const BridgeWatchdogSettings& bridge,
                                   const PowerMonitorSettings& power) {
  if (s.enabled && isReservedOrUnsafePin(s.pin)) return reservedPinError(s.pin);
  if (watchdogPinsConflict(s.enabled, s.pin, bridge.enabled, bridge.pin)) {
    return pinClashError(s.pin, "Camera Bridge Watchdog");
  }
  if (watchdogPinsConflict(s.enabled, s.pin, power.enabled, power.pin)) {
    return pinClashError(s.pin, "220V Power Monitor");
  }
  return "";
}

String validateBridgeWatchdogSettings(const BridgeWatchdogSettings& s, const NetWatchdogSettings& net,
                                      const PowerMonitorSettings& power) {
  if (s.enabled && (s.cameraA.length() == 0 || s.cameraB.length() == 0)) {
    return "Both Camera A and Camera B must be selected. Not saved.";
  }
  if (s.enabled && s.cameraA.equalsIgnoreCase(s.cameraB)) {
    return "Camera A and Camera B must be two different cameras. Not saved.";
  }
  if (s.enabled && isReservedOrUnsafePin(s.pin)) return reservedPinError(s.pin);
  if (watchdogPinsConflict(s.enabled, s.pin, net.enabled, net.pin)) return pinClashError(s.pin, "Internet Watchdog");
  if (watchdogPinsConflict(s.enabled, s.pin, power.enabled, power.pin)) {
    return pinClashError(s.pin, "220V Power Monitor");
  }
  return "";
}

String validatePowerMonitorSettings(const PowerMonitorSettings& s, const NetWatchdogSettings& net,
                                    const BridgeWatchdogSettings& bridge) {
  if (s.enabled && isReservedOrUnsafePin(s.pin)) return reservedPinError(s.pin);
  if (watchdogPinsConflict(s.enabled, s.pin, net.enabled, net.pin)) return pinClashError(s.pin, "Internet Watchdog");
  if (watchdogPinsConflict(s.enabled, s.pin, bridge.enabled, bridge.pin)) {
    return pinClashError(s.pin, "Camera Bridge Watchdog");
  }
  return "";
}
