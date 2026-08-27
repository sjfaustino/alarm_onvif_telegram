#include "format_utils.h"
#include <cctype>
#include <cstdio>

String formatUptime(unsigned long ms) {
  unsigned long totalSec = ms / 1000UL;
  unsigned long days  = totalSec / 86400UL;
  unsigned long hours = (totalSec % 86400UL) / 3600UL;
  unsigned long mins  = (totalSec % 3600UL) / 60UL;
  String s;
  if (days > 0) s += String(days) + "d ";
  s += String(hours) + "h " + String(mins) + "m";
  return s;
}

String formatElapsedSince(unsigned long eventMs, unsigned long nowMs) {
  unsigned long elapsed = nowMs - eventMs;
  if (elapsed < 60000UL) return "just now";
  return formatUptime(elapsed) + " ago";
}

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      default:   out += c;        break;
    }
  }
  return out;
}

String urlEncode(const String& s) {
  String out;
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

String extractHost(const String& url) {
  int schemeEnd = url.indexOf("://");
  int start = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', start);
  int end = (pathStart >= 0) ? pathStart : (int)url.length();
  return url.substring(start, end);
}
