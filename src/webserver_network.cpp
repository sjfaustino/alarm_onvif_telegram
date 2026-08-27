#include "webserver_network.h"
#include "network_store.h"
#include "format_utils.h"
#include <WiFi.h>
#include <cctype>

String renderNetworkPanel() {
  WifiCredentials creds = loadWifiCredentials();

  String connectedRole;
  if (WiFi.SSID() == creds.primary.ssid) connectedRole = " (primary)";
  else if (creds.backup.ssid.length() > 0 && WiFi.SSID() == creds.backup.ssid) connectedRole = " (backup)";

  String html = "<h1>Network</h1>";
  html += "<table>";
  html += "<tr><th>Connected SSID</th><td>" + htmlEscape(WiFi.SSID()) + connectedRole + "</td></tr>";
  html += "<tr><th>IP address</th><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><th>MAC address</th><td>" + WiFi.macAddress() + "</td></tr>";
  html += "<tr><th>Signal (RSSI)</th><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  html += "<tr><th>mDNS address</th><td>http://" + htmlEscape(creds.hostname) + ".local/</td></tr>";
  html += "<tr><th>Uptime</th><td>" + formatUptime(millis()) + "</td></tr>";
  html += "</table>";

  html += "<fieldset><legend>Primary WiFi</legend><form method=\"POST\" action=\"/network/save\">";
  html += "<label>SSID<input type=\"text\" name=\"ssid\" value=\"" + htmlEscape(creds.primary.ssid) + "\" required></label>";
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

  // Applies to whichever of primary/backup ends up connecting - see
  // network_store.h's comment on WifiCredentials::useStaticIP.
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

  // DHCP shows the board's live settings, grayed out; Static shows the
  // stored values, editable. Disabled inputs don't submit, so switching to
  // DHCP and saving correctly leaves the stored static values untouched.
  html += "<script>";
  html += "var netLive={ip:'" + WiFi.localIP().toString() + "',subnet:'" + WiFi.subnetMask().toString() +
          "',gateway:'" + WiFi.gatewayIP().toString() + "',dns:'" + WiFi.dnsIP().toString() + "'};";
  html += "var netStatic={ip:'" + creds.staticIP + "',subnet:'" + creds.staticSubnet +
          "',gateway:'" + creds.staticGateway + "',dns:'" + creds.staticDNS + "'};";
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
  html += "<label>Resync interval, minutes<input type=\"text\" name=\"ntpSyncMinutes\" value=\"" +
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
  return html;
}

// mDNS hostnames only support letters, digits, and hyphens - strip anything
// else rather than rejecting the whole save, so a stray pasted space or dot
// doesn't produce a hostname that silently fails to resolve.
static String sanitizeHostname(const String& raw) {
  String out;
  out.reserve(raw.length());
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (isalnum((unsigned char)c) || c == '-') out += c;
  }
  return out;
}

void handleSaveNetwork(PsychicRequest* request, String& banner) {
  WifiCredentials creds = loadWifiCredentials(); // current values, so a blank field keeps them

  String ssid = request->getParam("ssid", "");
  ssid.trim();
  if (ssid.length() > 0) creds.primary.ssid = ssid;

  String password = request->getParam("password", "");
  if (password.length() > 0) creds.primary.password = password;

  // Backup SSID isn't "blank keeps current" like the passwords - it's the
  // only way to disable a configured backup, so blank clears both fields.
  String backupSsid = request->getParam("backupSsid", "");
  backupSsid.trim();
  if (backupSsid.length() == 0) {
    creds.backup.ssid = "";
    creds.backup.password = "";
  } else {
    creds.backup.ssid = backupSsid;
    String backupPassword = request->getParam("backupPassword", "");
    if (backupPassword.length() > 0) creds.backup.password = backupPassword;
  }

  String hostname = sanitizeHostname(request->getParam("hostname", ""));
  if (hostname.length() > 0) creds.hostname = hostname;

  if (creds.primary.ssid.length() == 0) {
    banner = "Primary SSID is required - not saved.";
    return;
  }

  // Static fields are disabled (never submitted) when DHCP is selected -
  // only touch/validate them when the form actually submitted ipMode=static.
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
  if (ntpMinutes > 0) creds.ntpSyncIntervalMs = (unsigned long)ntpMinutes * 60000UL;
  // else keep whatever was already stored - a blank/zero/negative field
  // shouldn't produce a 0ms (hammer-the-server) resync interval.

  // A blank field is a valid choice in its own right (UTC, no TZ applied) -
  // no "keep previous value" fallback needed here, unlike the fields above.
  String posixTz = request->getParam("posixTz", "");
  posixTz.trim();
  creds.posixTz = posixTz;

  saveWifiCredentials(creds);
  banner = "Saved - reboot the board to apply the new network configuration.";
}
