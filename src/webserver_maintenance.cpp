#include "webserver_maintenance.h"
#include "net_watchdog.h"
#include "net_watchdog_logic.h" // isReservedOrUnsafePin
#include "config.h" // NET_WATCHDOG_THRESHOLD_MAX_MS/NET_WATCHDOG_PULSE_MAX_MS
#include "format_utils.h" // formatElapsedSince

// Internet-connectivity watchdog (relay-based router power-cycle) -
// optional, off by default, same "enable checkbox needs a reboot, status
// reflects what actually happened at boot" shape as the Network page's
// External RTC section. Lives on the Maintenance page, not Network: this
// is an automated-recovery action (this page's existing theme - the
// manual "Reboot now" button above), not the board's own network
// configuration.
static String renderNetWatchdogFieldset() {
  NetWatchdogStatus status = getNetWatchdogStatus();
  NetWatchdogSettings settings = loadNetWatchdogSettings();

  String html = "<fieldset><legend>Internet Watchdog</legend>";
  if (!status.settingEnabled) {
    html += "<p class=\"hint\">Disabled - for a board sitting behind a 4G/LTE router that sometimes "
            "loses its uplink and doesn't recover on its own, while the board's own WiFi link to it "
            "stays up the whole time. When enabled, a relay wired in series with the router's own "
            "power gets pulsed to force a power-cycle once an internet outage has lasted longer than "
            "the threshold below.</p>";
  } else if (!status.available) {
    html += "<p class=\"hint\">Enabled, but pin " + String(settings.pin) + " is reserved by another "
            "peripheral on this board or unsafe to use for general GPIO - pick a different pin below, "
            "save, then reboot. The watchdog is inactive in the meantime.</p>";
  } else if (status.outageInProgress) {
    html += "<p class=\"hint\">Internet outage in progress - first detected " +
            formatElapsedSince(status.outageStartMs, millis()) + ".</p>";
  } else {
    html += "<p class=\"hint\">Active on pin " + String(settings.pin) + " - no outage currently detected.</p>";
  }

  html += "<form method=\"POST\" action=\"/maintenance/net-watchdog/save\">";
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

String renderMaintenancePanel() {
  String html = "<h1>Maintenance</h1>";

  html += "<fieldset><legend>Reboot</legend>";
  html += "<form method=\"POST\" action=\"/maintenance/reboot\" "
          "onsubmit=\"return confirm('Reboot the board now? Every camera stops being monitored "
          "until it finishes reconnecting.');\">";
  html += "<p><button type=\"submit\" class=\"danger\">Reboot now</button></p>";
  html += "</form>";
  html += "<p class=\"hint\">Takes about 15-20 seconds to fully reconnect and resume monitoring - "
          "same as a power cycle, and just as disruptive to every camera's active subscription while "
          "it's down.</p>";
  html += "</fieldset>";

  html += renderNetWatchdogFieldset();
  return html;
}
