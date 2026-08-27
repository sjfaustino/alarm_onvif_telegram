#include "telegram_parse.h"

String jsonEscape(const String& in) {
  String out; out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

String jsonUnescape(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\\' && i + 1 < in.length()) {
      char n = in[++i];
      switch (n) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        default:   out += n;    break;
      }
    } else {
      out += c;
    }
  }
  return out;
}

std::vector<size_t> matchCamerasByPrefix(const CameraConfig cameras[], size_t numCameras, const String& needle) {
  String lowerNeedle = needle;
  lowerNeedle.toLowerCase();

  std::vector<size_t> matches;
  for (size_t i = 0; i < numCameras; i++) {
    if (!cameras[i].enabled) continue;
    String haystack = cameras[i].name;
    haystack.toLowerCase();
    if (haystack.startsWith(lowerNeedle)) matches.push_back(i);
  }
  return matches;
}
