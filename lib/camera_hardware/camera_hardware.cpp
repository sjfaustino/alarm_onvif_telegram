#include "camera_hardware.h"
#include "xml_helpers.h"

namespace {

// Each <prefix:localName ...>...</...> element's text, in order. Matches only
// whole names, so "Profiles" doesn't match "ProfilesExtension".
std::vector<String> elements(const String& xml, const String& localName) {
  std::vector<String> out;
  int from = 0;
  while (true) {
    int lt = xml.indexOf('<', from);
    if (lt < 0) break;
    int nameEnd = lt + 1;
    while (nameEnd < (int)xml.length() && xml[nameEnd] != '>' && xml[nameEnd] != ' ' && xml[nameEnd] != '/' &&
           xml[nameEnd] != '\t' && xml[nameEnd] != '\r' && xml[nameEnd] != '\n') {
      nameEnd++;
    }
    String tag = xml.substring(lt + 1, nameEnd);
    int colon = tag.indexOf(':');
    String local = colon >= 0 ? tag.substring(colon + 1) : tag;
    from = nameEnd;
    if (local != localName || tag.startsWith("/")) continue;
    int open = xml.indexOf('>', nameEnd);
    if (open < 0) break;
    if (xml[open - 1] == '/') { from = open + 1; continue; } // self-closing, no text
    // The matching close, counting nested elements of the same name.
    int depth = 1, scan = open + 1, close = -1;
    while (depth > 0) {
      int next = xml.indexOf('<', scan);
      if (next < 0) break;
      int end = xml.indexOf('>', next);
      if (end < 0) break;
      String t = xml.substring(next + 1, end);
      bool closing = t.startsWith("/");
      if (closing) t = t.substring(1);
      int sp = t.indexOf(' ');
      if (sp >= 0) t = t.substring(0, sp);
      int c = t.indexOf(':');
      String l = c >= 0 ? t.substring(c + 1) : t;
      if (l == localName && !t.endsWith("/")) {
        if (closing) depth--;
        else if (xml[end - 1] != '/') depth++;
        if (depth == 0) close = next;
      }
      scan = end + 1;
    }
    if (close < 0) break;
    out.push_back(xml.substring(open + 1, close));
    from = close + 1;
  }
  return out;
}

String first(const String& xml, const String& localName) {
  std::vector<String> e = elements(xml, localName);
  if (e.empty()) return "";
  String v = e[0];
  v.trim();
  return v;
}

void appendJoined(String& target, const String& value) {
  if (value.length() == 0) return;
  for (int start = 0; start <= (int)target.length();) {
    int comma = target.indexOf(", ", start);
    String item = target.substring(start, comma < 0 ? target.length() : comma);
    if (item == value) return;
    if (comma < 0) break;
    start = comma + 2;
  }
  target = target.length() ? target + ", " + value : value;
}

// %20 and friends, as some cameras encode spaces in scope names.
String percentDecode(const String& s) {
  String out;
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == '%' && i + 2 < (int)s.length() && isxdigit(s[i + 1]) && isxdigit(s[i + 2])) {
      out += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

}  // namespace

long long utcEpochSeconds(int year, int month, int day, int hour, int minute, int second) {
  // Days from civil (Howard Hinnant's algorithm).
  year -= month <= 2;
  long long era = (year >= 0 ? year : year - 399) / 400;
  long long yoe = year - era * 400;
  long long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long long days = era * 146097 + doe - 719468;
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

void parseDeviceInformation(const String& xml, CameraHardwareInfo& info) {
  info.manufacturer = first(xml, "Manufacturer");
  info.model = first(xml, "Model");
  info.firmwareVersion = first(xml, "FirmwareVersion");
  info.serialNumber = first(xml, "SerialNumber");
  info.hardwareId = first(xml, "HardwareId");
}

void parseNetworkInterfaces(const String& xml, CameraHardwareInfo& info) {
  std::vector<String> ifaces = elements(xml, "NetworkInterfaces");
  if (ifaces.empty()) return;
  const String& iface = ifaces[0];
  info.macAddress = first(iface, "HwAddress");
  info.macAddress.toUpperCase();

  String ipv4 = first(iface, "IPv4");
  String config = first(ipv4, "Config");
  bool dhcp = first(config, "DHCP").equalsIgnoreCase("true");
  String block = first(config, dhcp ? "FromDHCP" : "Manual");
  if (block.length() == 0) block = first(config, dhcp ? "Manual" : "FromDHCP");
  String address = first(block, "Address");
  String prefix = first(block, "PrefixLength");
  info.ipv4 = address.length() && prefix.length() ? address + "/" + prefix : address;
  if (config.length() > 0) info.ipMode = dhcp ? "DHCP" : "static";

  String oper = first(first(iface, "Link"), "OperSettings");
  String speed = first(oper, "Speed");
  String duplex = first(oper, "Duplex");
  if (speed.length() > 0 && speed != "0") {
    info.link = speed + " Mbit/s";
    if (duplex.length() > 0) {
      duplex.toLowerCase();
      info.link += ", " + duplex + " duplex";
    }
  }
}

void parseScopes(const String& xml, CameraHardwareInfo& info) {
  static const char* kPrefix = "onvif://www.onvif.org/";
  for (const String& raw : elements(xml, "ScopeItem")) {
    String item = raw;
    item.trim();
    if (!item.startsWith(kPrefix)) continue;
    String rest = item.substring(strlen(kPrefix));
    int slash = rest.indexOf('/');
    if (slash < 0) continue;
    String key = rest.substring(0, slash);
    String value = percentDecode(rest.substring(slash + 1));
    key.toLowerCase();
    if (key == "name") appendJoined(info.scopeName, value);
    else if (key == "location") appendJoined(info.scopeLocation, value);
    else if (key == "hardware") appendJoined(info.scopeHardware, value);
  }
}

void parseSystemDateAndTime(const String& xml, CameraHardwareInfo& info) {
  String type = first(xml, "DateTimeType");
  if (type.length() > 0) info.timeSource = type;
  info.timeZone = first(first(xml, "TimeZone"), "TZ");
  String utc = first(xml, "UTCDateTime");
  String date = first(utc, "Date");
  String time = first(utc, "Time");
  String year = first(date, "Year");
  if (year.length() == 0 || first(time, "Hour").length() == 0) return;
  info.clockUtcEpoch = utcEpochSeconds(year.toInt(), first(date, "Month").toInt(), first(date, "Day").toInt(),
                                       first(time, "Hour").toInt(), first(time, "Minute").toInt(),
                                       first(time, "Second").toInt());
}

void parseVideoStreams(const String& xml, CameraHardwareInfo& info) {
  info.videoStreams.clear();
  for (const String& profile : elements(xml, "Profiles")) {
    String vec = first(profile, "VideoEncoderConfiguration");
    if (vec.length() == 0) continue;
    // The profile's own name, not the encoder's (which comes after it).
    String name = first(profile.substring(0, profile.indexOf("VideoSourceConfiguration") > 0
                                                  ? profile.indexOf("VideoSourceConfiguration")
                                                  : profile.length()),
                        "Name");
    String line = (name.length() ? name : String("Stream")) + ": " + first(vec, "Encoding");
    String resolution = first(vec, "Resolution");
    String w = first(resolution, "Width"), h = first(resolution, "Height");
    if (w.length() && h.length()) line += " " + w + "x" + h;
    String rate = first(vec, "RateControl");
    String fps = first(rate, "FrameRateLimit"), kbps = first(rate, "BitrateLimit");
    if (fps.length()) line += ", " + fps + " fps";
    if (kbps.length()) line += ", " + kbps + " kbit/s";
    info.videoStreams.push_back(line);
  }
}
