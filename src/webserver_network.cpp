#include "webserver_network.h"
#include "network_store.h"
#include "config.h" // NTP_SYNC_MAX_MINUTES
#include "format_utils.h"
#include "wifi_scan.h"
#include "webserver_html.h" // DiscoveryResultRow, renderDiscoveryResultsTable
#include "background_job.h" // BackgroundJob<T>
#include "rtc_store.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <cctype>
#include <time.h>

// Live connection rows, shown under whichever network is connected.
static String renderLiveConnectionRows() {
  String html;
  html += "<tr><th>IP address</th><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><th>MAC address</th><td>" + WiFi.macAddress() + "</td></tr>";
  html += "<tr><th>Signal (RSSI)</th><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  html += "<tr><th>Gateway</th><td>" + WiFi.gatewayIP().toString() + "</td></tr>";
  html += "<tr><th>Subnet mask</th><td>" + WiFi.subnetMask().toString() + "</td></tr>";
  html += "<tr><th>DNS server</th><td>" + WiFi.dnsIP().toString() + "</td></tr>";
  return html;
}

// Optional DS3231 RTC; enabling needs a reboot and the status shows what
// happened at boot. Lives here with the other clock settings.
static String renderRtcFieldset() {
  RtcStatus status = getRtcStatus();

  String html = "<fieldset><legend>External RTC</legend>";
  if (!status.settingEnabled) {
    html += "<p class=\"hint\">No external RTC configured - the system clock relies on NTP only, "
            "which means it has no accurate time at all until WiFi connects and a sync completes. "
            "An optional DS3231 module (wired to RTC_SDA_PIN/RTC_SCL_PIN in config.h) lets the board "
            "seed a roughly-correct clock immediately at boot instead.</p>";
  } else if (!status.available) {
    html += "<p class=\"hint\">External RTC is enabled, but no chip ACKed at boot on the configured "
            "I2C pins - check the wiring and RTC_SDA_PIN/RTC_SCL_PIN/DS3231_I2C_ADDR in "
            "<code>config.h</code>, then reboot. The system clock is relying on NTP only in the "
            "meantime.</p>";
  } else {
    struct tm rtcTime;
    if (readRtcTime(&rtcTime)) {
      char buf[25];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &rtcTime);
      time_t now = time(nullptr);
      struct tm systemTime;
      gmtime_r(&now, &systemTime);
      char sysBuf[25];
      strftime(sysBuf, sizeof(sysBuf), "%Y-%m-%d %H:%M:%S", &systemTime);
      html += "<table>";
      html += "<tr><th>RTC reading</th><td>" + String(buf) + " UTC</td></tr>";
      html += "<tr><th>System clock</th><td>" + String(sysBuf) + " UTC</td></tr>";
      html += "</table>";
    } else {
      html += "<p class=\"hint\">External RTC is active, but reading its current time just failed - "
              "see the Serial log.</p>";
    }
  }

  html += "<form method=\"POST\" action=\"/network/rtc/save\">";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
          String(status.settingEnabled ? " checked" : "") + "> Use an external RTC module</label>";
  html += "<p><button type=\"submit\">Save</button></p></form>";
  html += "<p class=\"hint\">Takes effect after a reboot.</p>";
  html += "</fieldset>";
  return html;
}

String renderNetworkPanel(const String& prefillSsid) {
  WifiCredentials creds = loadWifiCredentials();

  // Status uses the stored primary, never the prefill: a scan's Add link once
  // made the real primary's status disappear.
  bool primaryConnected = WiFi.status() == WL_CONNECTED && WiFi.SSID() == creds.primary.ssid;
  bool backupConnected = WiFi.status() == WL_CONNECTED && creds.backup.ssid.length() > 0 &&
                          WiFi.SSID() == creds.backup.ssid;
  String formPrimarySsid = prefillSsid.length() > 0 ? prefillSsid : creds.primary.ssid;

  String html = "<h1>Network</h1>";

  // Live details go under whichever fieldset is actually connected.
  html += "<fieldset><legend>Primary</legend><table>";
  html += "<tr><th>SSID</th><td>" + htmlEscape(creds.primary.ssid) + "</td></tr>";
  html += "<tr><th>Status</th><td>" + String(primaryConnected ? "Connected" : "Not currently connected") +
          "</td></tr>";
  if (primaryConnected) html += renderLiveConnectionRows();
  html += "</table></fieldset>";

  html += "<fieldset><legend>Backup</legend>";
  if (creds.backup.ssid.length() == 0) {
    html += "<p class=\"hint\">No backup network configured.</p>";
  } else {
    html += "<table><tr><th>SSID</th><td>" + htmlEscape(creds.backup.ssid) + "</td></tr>";
    html += "<tr><th>Status</th><td>" + String(backupConnected ? "Connected" : "Not currently connected") +
            "</td></tr>";
    if (backupConnected) html += renderLiveConnectionRows();
    html += "</table>";
  }
  html += "</fieldset>";

  html += "<table>";
  html += "<tr><th>mDNS address</th><td>http://" + htmlEscape(creds.hostname) + ".local/</td></tr>";
  html += "<tr><th>Uptime</th><td>" + formatUptime(millis()) + "</td></tr>";
  html += "</table>";

  html += "<form method=\"POST\" action=\"/network/scan\">"
          "<p><button type=\"submit\">Search WiFi networks</button></p></form>";
  html += "<p class=\"hint\">Scans for nearby networks and lists what's found - click Add next to one "
          "to fill in its name below (the password still needs to be typed in by hand). Runs in the "
          "background; reload this page after clicking to see results. The scan briefly interrupts "
          "this board's own WiFi traffic while it hops channels, so the dashboard may pause for a "
          "moment around when it runs.</p>";
  html += renderWifiScanStatus();

  html += "<fieldset><legend>Primary WiFi</legend><form method=\"POST\" action=\"/network/save\">";
  html += "<label>SSID<input type=\"text\" name=\"ssid\" value=\"" + htmlEscape(formPrimarySsid) + "\" required></label>";
  html += "<label>Password (leave blank to keep the current password)"
          "<input type=\"password\" name=\"password\" placeholder=\"(unchanged)\"></label>";

  html += "<label style=\"margin-top:20px;\">Backup SSID (optional - tried if primary doesn't connect "
          "within 30s; leave blank to disable)<input type=\"text\" name=\"backupSsid\" value=\"" +
          htmlEscape(creds.backup.ssid) + "\"></label>";
  html += "<label>Backup password (leave blank to keep the current one, if backup SSID is unchanged)"
          "<input type=\"password\" name=\"backupPassword\" placeholder=\"(unchanged)\"></label>";
  html += "<p class=\"hint\">If the backup connects when primary doesn't, it's promoted to primary "
          "(and primary demoted to backup) automatically, so future boots try whichever network "
          "actually works first.</p>";

  html += "<label style=\"margin-top:20px;\">Hostname - letters, digits, hyphens only, no spaces or dots "
          "(reachable at http://&lt;hostname&gt;.local/)"
          "<input type=\"text\" name=\"hostname\" value=\"" + htmlEscape(creds.hostname) + "\" required></label>";

  html += "<div style=\"margin-top:20px;\"><label class=\"checkbox\">"
          "<input type=\"radio\" name=\"ipMode\" value=\"dhcp\" id=\"ipModeDhcp\"" +
          String(creds.useStaticIP ? "" : " checked") + "> DHCP</label>";
  html += "<label class=\"checkbox\"><input type=\"radio\" name=\"ipMode\" value=\"static\" id=\"ipModeStatic\"" +
          String(creds.useStaticIP ? " checked" : "") + "> Static IP</label></div>";
  html += "<label>IP address<input type=\"text\" name=\"staticIP\" id=\"staticIP\"></label>";
  html += "<label>Subnet mask<input type=\"text\" name=\"staticSubnet\" id=\"staticSubnet\"></label>";
  html += "<label>Gateway<input type=\"text\" name=\"staticGateway\" id=\"staticGateway\"></label>";
  html += "<label>DNS server (optional - falls back to the gateway if left blank)"
          "<input type=\"text\" name=\"staticDNS\" id=\"staticDNS\"></label>";

  // DHCP shows live values greyed out; Static shows stored values. Disabled
  // inputs don't submit, so saving in DHCP mode keeps the stored static
  // values.
  html += "<script>";
  // netLive is digits and dots only. netStatic is JS-escaped because an
  // imported config can put anything in those fields - otherwise script
  // injection.
  html += "var netLive={ip:'" + WiFi.localIP().toString() + "',subnet:'" + WiFi.subnetMask().toString() +
          "',gateway:'" + WiFi.gatewayIP().toString() + "',dns:'" + WiFi.dnsIP().toString() + "'};";
  html += "var netStatic={ip:'" + jsSingleQuoteEscape(creds.staticIP) + "',subnet:'" +
          jsSingleQuoteEscape(creds.staticSubnet) + "',gateway:'" + jsSingleQuoteEscape(creds.staticGateway) +
          "',dns:'" + jsSingleQuoteEscape(creds.staticDNS) + "'};";
  html += "var netFieldIds={ip:'staticIP',subnet:'staticSubnet',gateway:'staticGateway',dns:'staticDNS'};"
          "function applyIpMode(){"
          "var isStatic=document.getElementById('ipModeStatic').checked;"
          "var vals=isStatic?netStatic:netLive;"
          "for(var k in netFieldIds){"
          "var el=document.getElementById(netFieldIds[k]);"
          "el.disabled=!isStatic;el.value=vals[k];}}"
          "document.getElementById('ipModeDhcp').addEventListener('change',applyIpMode);"
          "document.getElementById('ipModeStatic').addEventListener('change',applyIpMode);"
          "applyIpMode();";
  html += "</script>";

  html += "<label style=\"margin-top:20px;\">NTP server (always uses UDP port 123 - not configurable "
          "on this platform)<input type=\"text\" name=\"ntpServer\" value=\"" +
          htmlEscape(creds.ntpServer) + "\" required></label>";
  html += "<label>Resync interval, minutes, max " + String(NTP_SYNC_MAX_MINUTES) +
          "<input type=\"text\" name=\"ntpSyncMinutes\" value=\"" +
          String(creds.ntpSyncIntervalMs / 60000UL) + "\"></label>";

  html += "<label style=\"margin-top:20px;\">POSIX TZ string for local time in Telegram alert photo "
          "captions (optional; leave blank for UTC) - the board's own clock always stays UTC (ONVIF "
          "requires it), this only affects what's shown in captions, and it auto-adjusts for daylight "
          "saving since the rule carries its own DST dates. Look yours up at "
          "https://github.com/nayarsystems/posix_tz_db - e.g. mainland Portugal: "
          "WET0WEST,M3.5.0/1,M10.5.0"
          "<input type=\"text\" name=\"posixTz\" value=\"" + htmlEscape(creds.posixTz) + "\"></label>";

  html += "<p><button type=\"submit\">Save</button></p></form></fieldset>";

  html += "<p class=\"hint\">Saving updates storage immediately, but only takes effect after "
          "the board reboots - a live change could drop it off the network with no way back to "
          "this page if the new credentials are wrong.</p>";

  html += renderRtcFieldset();
  return html;
}

void handleSaveNetwork(PsychicRequest* request, String& banner) {
  WifiCredentials creds = loadWifiCredentials(); // current values, so a blank field keeps them

  String ssid = request->getParam("ssid", "");
  ssid.trim();
  String password = request->getParam("password", "");
  // A new SSID with a blank password would pair the old password with the new
  // network and strand the board, so require one whenever the SSID changes.
  if (ssid.length() > 0 && ssid != creds.primary.ssid && password.length() == 0) {
    banner = "Primary SSID changed - its password is required too (an old password would otherwise be "
             "paired with the new network). Not saved.";
    return;
  }
  if (ssid.length() > 0) creds.primary.ssid = ssid;
  if (password.length() > 0) creds.primary.password = password;

  // Blank backup SSID removes the backup (unlike blank passwords, which keep
  // the current one).
  String backupSsid = request->getParam("backupSsid", "");
  backupSsid.trim();
  if (backupSsid.length() == 0) {
    creds.backup.ssid = "";
    creds.backup.password = "";
  } else {
    String backupPassword = request->getParam("backupPassword", "");
    if (backupSsid != creds.backup.ssid && backupPassword.length() == 0) {
      banner = "Backup SSID changed - its password is required too (an old password would otherwise be "
               "paired with the new network). Not saved.";
      return;
    }
    creds.backup.ssid = backupSsid;
    if (backupPassword.length() > 0) creds.backup.password = backupPassword;
  }

  String hostname = sanitizeHostname(request->getParam("hostname", ""));
  if (hostname.length() > 0) creds.hostname = hostname;

  if (creds.primary.ssid.length() == 0) {
    banner = "Primary SSID is required - not saved.";
    return;
  }

  // Static fields only submit (and are validated) in static mode.
  String ipMode = request->getParam("ipMode", "dhcp");
  creds.useStaticIP = (ipMode == "static");
  if (creds.useStaticIP) {
    String ip = request->getParam("staticIP", "");
    String subnet = request->getParam("staticSubnet", "");
    String gateway = request->getParam("staticGateway", "");
    String dns = request->getParam("staticDNS", "");
    ip.trim(); subnet.trim(); gateway.trim(); dns.trim();

    IPAddress parsed;
    if (ip.length() == 0 || subnet.length() == 0 || gateway.length() == 0 ||
        !parsed.fromString(ip) || !parsed.fromString(subnet) || !parsed.fromString(gateway) ||
        (dns.length() > 0 && !parsed.fromString(dns))) {
      banner = "Static IP, subnet mask, and gateway must be valid addresses (DNS is optional) - not saved.";
      return;
    }
    creds.staticIP = ip;
    creds.staticSubnet = subnet;
    creds.staticGateway = gateway;
    creds.staticDNS = dns;
  }

  String ntpServer = request->getParam("ntpServer", "");
  ntpServer.trim();
  if (ntpServer.length() > 0) creds.ntpServer = ntpServer;

  long ntpMinutes = request->getParam("ntpSyncMinutes", "60").toInt();
  // Capped at 30 days (minutes * 60000 overflows 32 bits). Re-clamped at use.
  if (ntpMinutes > (long)NTP_SYNC_MAX_MINUTES) ntpMinutes = (long)NTP_SYNC_MAX_MINUTES;
  if (ntpMinutes > 0) creds.ntpSyncIntervalMs = (unsigned long)ntpMinutes * 60000UL;
  // else keep the stored value; 0 would resync continuously.

  // Blank is valid (UTC).
  String posixTz = request->getParam("posixTz", "");
  posixTz.trim();
  creds.posixTz = posixTz;

  if (!saveWifiCredentials(creds)) {
    banner = "Failed to save - NVS write error (see Serial log). Network settings were NOT changed.";
    return;
  }
  banner = "Saved - reboot the board to apply the new network configuration.";
}

// ============================================================
// WiFi network scan.
// ============================================================

// Distinguishes a failed scan from a quiet neighbourhood; both used to read
// "No networks found".
struct WifiScanOutcome {
  bool scanFailed = false;
  std::vector<WifiScanResult> networks;
};

static BackgroundJob<WifiScanOutcome> g_wifiScanJob;

// Let the "Scanning..." response go out before channel hopping starts
// disrupting the radio.
static const unsigned long kScanStartDelayMs = 500;

static WifiScanOutcome runWifiScan() {
  delay(kScanStartDelayMs);

  WifiScanOutcome outcome;
  int16_t n = WiFi.scanNetworks();
  if (n < 0) {
    outcome.scanFailed = true;
  } else if (n > 0) {
    std::vector<WifiScanResult> raw;
    raw.reserve(n);
    for (int16_t i = 0; i < n; i++) {
      WifiScanResult r;
      r.ssid = WiFi.SSID(i);
      r.rssi = WiFi.RSSI(i);
      r.encrypted = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      raw.push_back(r);
    }
    outcome.networks = dedupeSortWifiScanResults(raw);
  }
  WiFi.scanDelete(); // frees the scan's own internal buffer regardless of n
  return outcome;
}

static void wifiScanTask(void*) {
  g_wifiScanJob.finish(runWifiScan());
  vTaskDelete(nullptr);
}

BackgroundJobStartOutcome startWifiScanAsync() {
  if (!g_wifiScanJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one scan at a time - a second click while one's in flight is a no-op
  BaseType_t created = xTaskCreate(wifiScanTask, "wifiScan", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // Undo tryStart() so the job isn't stuck in progress.
    g_wifiScanJob.cancelStart();
    Serial.println("[webserver_network] ERROR: failed to start the WiFi scan task (out of memory?) - "
                    "try again once memory frees up.");
    return BackgroundJobStartOutcome::FailedToStart;
  }
  return BackgroundJobStartOutcome::Started;
}

String renderWifiScanStatus() {
  auto st = g_wifiScanJob.status();
  if (st.inProgress) {
    return "<p class=\"hint\">Scanning for WiFi networks - reload this page in a few seconds to see "
           "results. This board's own WiFi traffic may briefly pause while the scan runs.</p>";
  }
  if (!st.hasResult) return "";
  if (st.result.scanFailed) {
    return "<p class=\"hint\">The WiFi scan didn't complete (a radio/driver hiccup) - try again.</p>";
  }
  if (st.result.networks.empty()) {
    return "<p class=\"hint\">No networks found.</p>";
  }

  std::vector<DiscoveryResultRow> rows;
  for (auto& n : st.result.networks) {
    rows.push_back({{n.ssid, String(n.rssi) + " dBm", n.encrypted ? "Encrypted" : "Open"},
                     {{"prefillSsid", n.ssid}}});
  }
  return "<p>Networks found:</p>" + renderDiscoveryResultsTable({"SSID", "Signal", "Security"}, "/network", rows);
}

bool networkJobsInProgress() {
  return g_wifiScanJob.status().inProgress;
}
