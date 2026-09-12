#include "webserver_hardware.h"
#include "net_watchdog.h"
#include "bridge_watchdog.h"
#include "power_monitor.h"
#include "net_watchdog_logic.h" // isReservedOrUnsafePin
#include "config.h" // NET_WATCHDOG_THRESHOLD_MAX_MS/NET_WATCHDOG_PULSE_MAX_MS and BRIDGE_/POWER_MONITOR_ counterparts
#include "format_utils.h" // formatElapsedSince, htmlEscape

// Internet-connectivity watchdog (relay-based router power-cycle) -
// optional, off by default, same "enable checkbox needs a reboot, status
// reflects what actually happened at boot" shape as the Network page's
// External RTC section.
String renderInternetWatchdogPage() {
  NetWatchdogStatus status = getNetWatchdogStatus();
  NetWatchdogSettings settings = loadNetWatchdogSettings();

  String html = "<h1>Hardware - Internet Watchdog</h1>";
  html += "<fieldset><legend>Internet Watchdog</legend>";
  if (!status.settingEnabled) {
    html += "<p class=\"hint\">Disabled - for a board sitting behind a 4G/LTE router that sometimes "
            "loses its uplink and doesn't recover on its own, while the board's own WiFi link to it "
            "stays up the whole time. When enabled, a relay wired in series with the router's own "
            "power gets pulsed to force a power-cycle once an internet outage has lasted longer than "
            "the threshold below.</p>";
  } else if (!status.available) {
    html += "<p class=\"hint\">Enabled, but pin " + String(settings.pin) + " is reserved by another "
            "peripheral on this board, unsafe to use for general GPIO, or already used by the Camera "
            "Bridge Watchdog or 220V Power Monitor - pick a different pin below, save, then reboot. "
            "The watchdog is inactive in the meantime.</p>";
  } else if (status.outageInProgress) {
    html += "<p class=\"hint\">Internet outage in progress - first detected " +
            formatElapsedSince(status.outageStartMs, millis()) + ".</p>";
  } else {
    html += "<p class=\"hint\">Active on pin " + String(settings.pin) + " - no outage currently detected.</p>";
  }

  html += "<form method=\"POST\" action=\"/hardware/internet/save\">";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
          String(settings.enabled ? " checked" : "") + "> Use a relay to power-cycle the router on an "
          "internet outage</label>";
  html += "<label>Relay GPIO pin"
          "<input type=\"text\" name=\"pin\" value=\"" + String(settings.pin) + "\"></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"activeLow\"" +
          String(settings.activeLow ? " checked" : "") + "> Relay module is active-low (LOW energizes "
          "it - true for most common relay boards)</label>";
  html += "<label>Outage threshold before pulsing, minutes"
          "<input type=\"text\" name=\"outageThresholdMinutes\" value=\"" +
          String(settings.outageThresholdMs / 60000UL) + "\"></label>";
  html += "<label>Pulse duration, seconds"
          "<input type=\"text\" name=\"pulseSeconds\" value=\"" +
          String(settings.pulseDurationMs / 1000UL) + "\"></label>";
  html += "<p><button type=\"submit\">Save</button></p></form>";
  html += "<p class=\"hint\">The enable checkbox and pin need a reboot to apply (pinMode() is only set "
          "up at boot) - the threshold and pulse duration take effect on the very next check.</p>";
  html += "</fieldset>";
  return html;
}

// Camera bridge watchdog (relay-based power-cycle for a local wireless
// bridge carrying two specific cameras) - same shape as
// renderInternetWatchdogPage above, plus one more status state: enabled
// but one of the configured camera names doesn't currently resolve.
String renderBridgeWatchdogPage(const std::vector<CameraConfig>* liveCameras) {
  BridgeWatchdogStatus status = getBridgeWatchdogStatus();
  BridgeWatchdogSettings settings = loadBridgeWatchdogSettings();

  String html = "<h1>Hardware - Camera Bridge Watchdog</h1>";
  html += "<fieldset><legend>Camera Bridge Watchdog</legend>";
  if (!status.settingEnabled) {
    html += "<p class=\"hint\">Disabled - for two cameras that sit behind a local wireless bridge "
            "that occasionally drops and needs a physical power reset to come back. When enabled, a "
            "relay wired to the bridge's own power gets pulsed once BOTH configured cameras have been "
            "OFFLINE for longer than the threshold below (a single camera going offline is far more "
            "likely to be that one camera's own problem than the bridge).</p>";
  } else if (!status.available) {
    html += "<p class=\"hint\">Enabled, but pin " + String(settings.pin) + " is reserved by another "
            "peripheral, unsafe to use for general GPIO, or already used by the Internet Watchdog or "
            "220V Power Monitor - pick a different pin below, save, then reboot. The watchdog is "
            "inactive in the meantime.</p>";
  } else if (!status.camerasResolved) {
    html += "<p class=\"hint\">Active on pin " + String(settings.pin) + ", but camera \"" +
            htmlEscape(settings.cameraA) + "\" or \"" + htmlEscape(settings.cameraB) + "\" doesn't "
            "currently match an enabled camera - check the names below (e.g. after a rename or "
            "delete). The watchdog can't evaluate the pair until this is fixed.</p>";
  } else if (status.outageInProgress) {
    html += "<p class=\"hint\">Bridge outage in progress - both cameras first went offline together " +
            formatElapsedSince(status.outageStartMs, millis()) + ".</p>";
  } else {
    html += "<p class=\"hint\">Active on pin " + String(settings.pin) + " - no outage currently detected.</p>";
  }

  html += "<form method=\"POST\" action=\"/hardware/bridge/save\">";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
          String(settings.enabled ? " checked" : "") + "> Use a relay to power-cycle the bridge when "
          "both cameras below are offline</label>";

  bool haveTwoCameras = liveCameras && liveCameras->size() >= 2;
  html += "<label>Camera A<select name=\"cameraA\"" + String(!haveTwoCameras ? " disabled" : "") + ">";
  if (!haveTwoCameras) {
    html += "<option value=\"\">Need at least 2 configured cameras</option>";
  } else {
    for (const auto& cam : *liveCameras) {
      String name = htmlEscape(cam.name);
      html += "<option value=\"" + name + "\"" +
              (cam.name.equalsIgnoreCase(settings.cameraA) ? " selected" : "") + ">" + name + "</option>";
    }
  }
  html += "</select></label>";

  html += "<label>Camera B<select name=\"cameraB\"" + String(!haveTwoCameras ? " disabled" : "") + ">";
  if (!haveTwoCameras) {
    html += "<option value=\"\">Need at least 2 configured cameras</option>";
  } else {
    for (const auto& cam : *liveCameras) {
      String name = htmlEscape(cam.name);
      html += "<option value=\"" + name + "\"" +
              (cam.name.equalsIgnoreCase(settings.cameraB) ? " selected" : "") + ">" + name + "</option>";
    }
  }
  html += "</select></label>";

  html += "<label>Relay GPIO pin"
          "<input type=\"text\" name=\"pin\" value=\"" + String(settings.pin) + "\"></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"activeLow\"" +
          String(settings.activeLow ? " checked" : "") + "> Relay module is active-low (LOW energizes "
          "it - true for most common relay boards)</label>";
  html += "<label>Outage threshold before pulsing, minutes"
          "<input type=\"text\" name=\"outageThresholdMinutes\" value=\"" +
          String(settings.outageThresholdMs / 60000UL) + "\"></label>";
  html += "<label>Pulse duration, seconds"
          "<input type=\"text\" name=\"pulseSeconds\" value=\"" +
          String(settings.pulseDurationMs / 1000UL) + "\"></label>";
  html += "<p><button type=\"submit\">Save</button></p></form>";
  html += "<p class=\"hint\">The enable checkbox and pin need a reboot to apply (pinMode() is only "
          "set up at boot) - which cameras to watch, the threshold, and the pulse duration all take "
          "effect on the very next check.</p>";
  html += "</fieldset>";
  return html;
}

// 220V mains power monitor - the reverse of the two relay watchdogs above:
// an INPUT, not an output. No threshold/pulse fields - there's nothing to
// pulse, just a reading to report and alert on.
String renderPowerMonitorPage() {
  PowerMonitorStatus status = getPowerMonitorStatus();
  PowerMonitorSettings settings = loadPowerMonitorSettings();

  String html = "<h1>Hardware - 220V Power Monitor</h1>";
  html += "<fieldset><legend>220V Power Monitor</legend>";
  if (!status.settingEnabled) {
    html += "<p class=\"hint\">Disabled - for a relay driven by a 220V-to-5V transformer, its NO "
            "(normally-open) contact wired to a pin on this board. While mains power is present the "
            "transformer energizes the relay, closing the contact; losing power opens it. Purely a "
            "sensor - the board and its router are themselves on a UPS, so nothing gets power-cycled "
            "here. When enabled, you're alerted at boot with the current reading, and again every time "
            "it changes.</p>";
  } else if (!status.available) {
    html += "<p class=\"hint\">Enabled, but pin " + String(settings.pin) + " is reserved by another "
            "peripheral, unsafe to use for general GPIO, or already used by the Internet Watchdog or "
            "Camera Bridge Watchdog - pick a different pin below, save, then reboot. The monitor is "
            "inactive in the meantime.</p>";
  } else {
    html += "<p class=\"hint\">Active on pin " + String(settings.pin) + " - mains power is currently " +
            (status.powerPresent ? "ON" : "OFF") + ".</p>";
  }

  html += "<form method=\"POST\" action=\"/hardware/power/save\">";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
          String(settings.enabled ? " checked" : "") + "> Monitor mains power via a relay's dry "
          "contact</label>";
  html += "<label>Sensor GPIO pin"
          "<input type=\"text\" name=\"pin\" value=\"" + String(settings.pin) + "\"></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"activeHigh\"" +
          String(settings.activeHigh ? " checked" : "") + "> Pin reads HIGH when power is present "
          "(leave unchecked for the common COM-&gt;GND, NO-&gt;pin wiring)</label>";
  html += "<p><button type=\"submit\">Save</button></p></form>";
  html += "<p class=\"hint\">The enable checkbox and pin need a reboot to apply (pinMode() is only "
          "set up at boot) - the wiring polarity takes effect on the very next check.</p>";
  html += "</fieldset>";
  return html;
}
