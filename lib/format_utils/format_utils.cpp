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
      // A single quote left un-escaped is a real hole, not just theory:
      // renderEditDeleteActions (lib/webserver_html) interpolates an
      // htmlEscape()d name into a single-quoted JS string inside an
      // onsubmit="return confirm('...')" attribute - the outer HTML
      // attribute is double-quoted (safe, '"' is escaped above), but
      // without this case a name containing a literal ' could still break
      // out of the inner JS string and inject arbitrary script that runs
      // when an admin clicks Delete on that row. &#39; (not &apos;, which
      // isn't valid in every HTML context) is the universally-safe form.
      case '\'': out += "&#39;";  break;
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

bool isSameOriginRequest(const String& origin, const String& referer, const String& host) {
  String source = origin.length() > 0 ? origin : referer;
  if (source.length() == 0) return true;
  return extractHost(source).equalsIgnoreCase(host);
}

String redactUrlSecrets(const String& url) {
  String out = url;
  int schemeEnd = out.indexOf("://");
  int start = schemeEnd >= 0 ? schemeEnd + 3 : 0;
  int pathStart = out.indexOf('/', start);
  int at = out.indexOf('@', start);
  if (at >= 0 && (pathStart < 0 || at < pathStart)) {
    int colon = out.indexOf(':', start);
    if (colon >= 0 && colon < at) out = out.substring(0, colon + 1) + "***" + out.substring(at);
  }
  int query = out.indexOf('?');
  if (query < 0) return out;
  static const char* kKeys[] = {"password", "pwd", "pass", "passwd", "loginpas", "psw"};
  int p = query + 1;
  while (p < (int)out.length()) {
    int amp = out.indexOf('&', p);
    int end = amp < 0 ? out.length() : amp;
    int eq = out.indexOf('=', p);
    if (eq >= 0 && eq < end) {
      String key = out.substring(p, eq);
      key.toLowerCase();
      for (const char* k : kKeys) {
        if (key == k) {
          out = out.substring(0, eq + 1) + "***" + out.substring(end);
          end = eq + 4;
          break;
        }
      }
    }
    p = end + 1;
  }
  return out;
}

String jsSingleQuoteEscape(const String& s) {
  String out = s;
  out.replace("\\", "\\\\"); // must run first - otherwise this would double-escape the backslashes just inserted below
  out.replace("'", "\\'");
  out.replace("\n", "\\n");
  out.replace("\r", "\\r");
  return out;
}
