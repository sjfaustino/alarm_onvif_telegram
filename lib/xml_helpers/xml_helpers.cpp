#include "xml_helpers.h"

String xmlEscape(const String& value) {
  String out;
  out.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default:   out += c;        break;
    }
  }
  return out;
}

String findElementByLocalName(const String& xml, const String& localName, int fromPosition) {
  String search1 = "<" + localName + ">";
  int p = xml.indexOf(search1, fromPosition);
  if (p >= 0) {
    int start = p + search1.length();
    int end = xml.indexOf("</" + localName + ">", start);
    if (end > start) {
      String result = xml.substring(start, end);
      result.trim();
      return result;
    }
  }
  String suffix = ":" + localName + ">";
  p = xml.indexOf(suffix, fromPosition);
  if (p >= 0) {
    int start = p + suffix.length();
    int end = xml.indexOf("</", start);
    if (end > start) {
      String result = xml.substring(start, end);
      result.trim();
      return result;
    }
  }
  return "";
}

String findAttributeValue(const String& xml, const String& elementName, const String& attributeName) {
  int element = xml.indexOf(elementName);
  if (element < 0) return "";
  int end = xml.indexOf(">", element);
  if (end < 0) return "";
  String section = xml.substring(element, end);
  String pattern = attributeName + "=\"";
  int p = section.indexOf(pattern);
  if (p < 0) return "";
  p += pattern.length();
  int q = section.indexOf("\"", p);
  if (q < 0) return "";
  return section.substring(p, q);
}

bool responseHasFault(const String& response) {
  return response.indexOf("<s:Fault") >= 0 || response.indexOf(":Fault") >= 0;
}
