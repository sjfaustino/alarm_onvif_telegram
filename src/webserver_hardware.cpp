#include "webserver_hardware.h"
#include "net_watchdog.h"
#include "bridge_watchdog.h"
#include "power_monitor.h"
#include "net_watchdog_logic.h" // isReservedOrUnsafePin
#include "config.h" // NET_WATCHDOG_THRESHOLD_MAX_MS/NET_WATCHDOG_PULSE_MAX_MS and BRIDGE_/POWER_MONITOR_ counterparts
#include "format_utils.h" // formatElapsedSince, htmlEscape
#include "webserver_internal.h"
#include "hardware_form.h"
#include "request_params.h"
#include "camera.h" // CameraState, CameraStateLock

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

  if (status.available) {
    html += "<form method=\"POST\" action=\"/hardware/internet/pulse\">"
            "<p><button type=\"submit\">Pulse relay now</button> "
            "<span class=\"hint\">Fires a test pulse immediately, without waiting for (or faking) a "
            "real outage - doesn't affect outage detection.</span></p></form>";
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

  if (status.available) {
    html += "<form method=\"POST\" action=\"/hardware/bridge/pulse\">"
            "<p><button type=\"submit\">Pulse relay now</button> "
            "<span class=\"hint\">Fires a test pulse immediately, without waiting for (or faking) a "
            "real outage - doesn't affect outage detection.</span></p></form>";
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
    html += "<p class=\"hint\">Pin " + String(settings.pin) + " currently reads " +
            (status.rawPinHigh ? "HIGH" : "LOW") + " (raw) - interpreted as power " +
            (status.powerPresent ? "PRESENT" : "ABSENT") + " given the wiring setting below. Use this "
            "live reading to verify your wiring/polarity choice right after flashing, without needing "
            "to physically cut mains power.</p>";
  }

  html += "<form method=\"POST\" action=\"/hardware/power/save\">";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
          String(settings.enabled ? " checked" : "") + "> Monitor mains power via a relay's dry "
          "contact</label>";
  html += "<label>Sensor GPIO pin"
          "<input type=\"text\" name=\"pin\" value=\"" + String(settings.pin) + "\"></label>";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"activeHigh\"" +
          String(settings.activeHigh ? " checked" : "") + "> Pin reads HIGH when power is present - "
          "leave unchecked for COM-&gt;GND, NO-&gt;pin wiring (pin pulled up internally); check it for "
          "COM-&gt;3.3V/VCC, NO-&gt;pin wiring instead (pin pulled down internally)</label>";
  html += "<p><button type=\"submit\">Save</button></p></form>";
  html += "<p class=\"hint\">Everything above needs a reboot to apply - pinMode() (which depends on "
          "both the pin and the wiring polarity, not just the pin) is only set up at boot, same as the "
          "enable checkbox.</p>";
  html += "</fieldset>";
  return html;
}

static void addRow(String& html, const char* label, const String& value) {
  if (value.length() == 0) return;
  html += "<tr><th style=\"text-align:left;white-space:nowrap\">" + String(label) + "</th><td>" + value + "</td></tr>";
}

static String formatClockSkew(long skewS) {
  long magnitude = skewS < 0 ? -skewS : skewS;
  if (magnitude <= 2) return "in step with the board";
  String amount = magnitude < 120 ? String(magnitude) + " s" : formatUptime((unsigned long)magnitude * 1000UL);
  return amount + (skewS > 0 ? " ahead of" : " behind") + " the board";
}

String renderCameraHardwarePage(const std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  String html = "<h1>Hardware - Cameras</h1>";
  html += "<p class=\"hint\">What each camera reports about itself over ONVIF, read when it connects and "
          "after an edit. Fields a camera doesn't report are left out.</p>";
  html += "<form method=\"POST\" action=\"/hardware/cameras/refresh\"><button type=\"submit\">Read again "
          "from all cameras</button></form>";
  if (!liveCameras || !liveStates) return html;

  unsigned long nowMs = millis();
  for (size_t i = 0; i < liveCameras->size() && i < liveStates->size(); i++) {
    const CameraConfig& cfg = (*liveCameras)[i];
    CameraState& st = (*liveStates)[i];
    html += "<fieldset><legend>" + htmlEscape(cfg.name) + "</legend>";
    if (!cfg.enabled || !st.stateMutex) {
      html += "<p class=\"hint\">Not monitored (disabled), so not read.</p></fieldset>";
      continue;
    }
    CameraHardwareInfo hw;
    unsigned long readMs;
    bool skewKnown;
    long skewS;
    String snapshotUri, streamUri, topics;
    {
      CameraStateLock lock(st);
      hw = st.hardware;
      readMs = st.hardwareReadMs;
      skewKnown = st.hardwareClockSkewKnown;
      skewS = st.hardwareClockSkewS;
      snapshotUri = st.snapshotUri;
      streamUri = st.streamUri;
      topics = st.supportedEventTopics;
    }
    if (readMs == 0) {
      html += "<p class=\"hint\">Not read yet - the camera hasn't answered. Reload once it's online.</p>"
              "</fieldset>";
      continue;
    }
    if (hw.model.length() == 0) {
      html += "<p class=\"hint\">The camera didn't answer GetDeviceInformation; what it did report is below.</p>";
    }
    html += "<table>";
    addRow(html, "Manufacturer", htmlEscape(hw.manufacturer));
    addRow(html, "Model", htmlEscape(hw.model));
    if (hw.hardwareId != hw.model) addRow(html, "Hardware ID", htmlEscape(hw.hardwareId));
    addRow(html, "Firmware", htmlEscape(hw.firmwareVersion));
    addRow(html, "Serial number", htmlEscape(hw.serialNumber));
    addRow(html, "MAC address", htmlEscape(hw.macAddress));
    addRow(html, "IP address",
           htmlEscape(hw.ipv4) + (hw.ipMode.length() ? " (" + htmlEscape(hw.ipMode) + ")" : String()));
    addRow(html, "Network link", htmlEscape(hw.link));
    addRow(html, "ONVIF name", htmlEscape(hw.scopeName));
    addRow(html, "ONVIF location", htmlEscape(hw.scopeLocation));
    if (hw.scopeHardware != hw.model) addRow(html, "ONVIF hardware", htmlEscape(hw.scopeHardware));
    String clock;
    if (hw.timeSource.length()) clock += "set by " + htmlEscape(hw.timeSource);
    if (hw.timeZone.length()) clock += (clock.length() ? ", time zone " : "time zone ") + htmlEscape(hw.timeZone);
    if (skewKnown) clock += (clock.length() ? ", " : "") + formatClockSkew(skewS);
    addRow(html, "Clock", clock);
    String streams;
    for (const String& s : hw.videoStreams) streams += (streams.length() ? "<br>" : "") + htmlEscape(s);
    addRow(html, "Video streams", streams);
    addRow(html, "Snapshot URL", htmlEscape(redactUrlSecrets(snapshotUri)));
    addRow(html, "Live stream URL", htmlEscape(redactUrlSecrets(streamUri)));
    addRow(html, "Event types", htmlEscape(topics));
    addRow(html, "Read", formatElapsedSince(readMs, nowMs));
    html += "</table></fieldset>";
  }
  return html;
}

void registerHardwareRoutes(PsychicHttpServer& server) {
  server.on("/hardware/cameras", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareCameras, "", renderCameraHardwarePage(g_liveCameras, g_liveStates))
                               .c_str());
  });

  server.on("/hardware/cameras/refresh", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    if (g_liveStates) {
      for (CameraState& st : *g_liveStates) {
        if (!st.stateMutex) continue;
        CameraStateLock lock(st);
        st.hardwareRefreshRequested = true;
      }
    }
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareCameras,
                                       "Reading again from every monitored camera - reload in a few seconds.",
                                       renderCameraHardwarePage(g_liveCameras, g_liveStates))
                               .c_str());
  });

  server.on("/hardware/internet", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareInternet, "", renderInternetWatchdogPage()).c_str());
  });

  server.on("/hardware/internet/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    NetWatchdogSettings settings = parseNetWatchdogForm(RequestParams(request));
    String banner = validateNetWatchdogSettings(settings, loadBridgeWatchdogSettings(), loadPowerMonitorSettings());
    if (banner.length() == 0) {
      banner = saveNetWatchdogSettings(settings)
          ? "Saved - the enable checkbox and pin need a reboot to apply; the threshold and pulse "
            "duration are active immediately."
          : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    }
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareInternet, banner, renderInternetWatchdogPage()).c_str());
  });

  server.on("/hardware/internet/pulse", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner = netWatchdogManualPulse()
        ? "Relay pulsed."
        : "Internet Watchdog isn't active (disabled, or the configured pin wasn't accepted at boot) - nothing to pulse.";
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareInternet, banner, renderInternetWatchdogPage()).c_str());
  });

  server.on("/hardware/bridge", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareBridge, "", renderBridgeWatchdogPage(g_liveCameras)).c_str());
  });

  server.on("/hardware/bridge/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    BridgeWatchdogSettings settings = parseBridgeWatchdogForm(RequestParams(request));
    String banner = validateBridgeWatchdogSettings(settings, loadNetWatchdogSettings(), loadPowerMonitorSettings());
    if (banner.length() == 0) {
      banner = saveBridgeWatchdogSettings(settings)
          ? "Saved - the enable checkbox, cameras, and pin need a reboot to apply; the threshold and "
            "pulse duration are active immediately."
          : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    }
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareBridge, banner, renderBridgeWatchdogPage(g_liveCameras)).c_str());
  });

  server.on("/hardware/bridge/pulse", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner = bridgeWatchdogManualPulse()
        ? "Relay pulsed."
        : "Camera Bridge Watchdog isn't active (disabled, or the configured pin wasn't accepted at boot) - nothing to pulse.";
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareBridge, banner, renderBridgeWatchdogPage(g_liveCameras)).c_str());
  });

  server.on("/hardware/power", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html",
                           renderShell(Tab::HardwarePower, "", renderPowerMonitorPage()).c_str());
  });

  server.on("/hardware/power/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    PowerMonitorSettings settings = parsePowerMonitorForm(RequestParams(request));
    String banner = validatePowerMonitorSettings(settings, loadNetWatchdogSettings(), loadBridgeWatchdogSettings());
    if (banner.length() == 0) {
      banner = savePowerMonitorSettings(settings)
          ? "Saved - the enable checkbox and pin need a reboot to apply; the wiring polarity is active "
            "immediately."
          : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    }
    return response->send(200, "text/html",
                           renderShell(Tab::HardwarePower, banner, renderPowerMonitorPage()).c_str());
  });
}
