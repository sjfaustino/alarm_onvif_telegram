#include <cstring>
#include "xml_helpers.h"
#include <cctype>

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
  if (p < 0) return "";
  int start = p + suffix.length();

  // Prefixed opening tag: the closing tag may drop the prefix, so compare each
  // "</...>"'s local name instead of the exact string.
  int searchFrom = start;
  while (true) {
    int closeStart = xml.indexOf("</", searchFrom);
    if (closeStart < 0) return "";
    int closeEnd = xml.indexOf('>', closeStart);
    if (closeEnd < 0) return "";

    String closeTagName = xml.substring(closeStart + 2, closeEnd);
    int colonPos = closeTagName.lastIndexOf(':');
    String closeLocalName = (colonPos >= 0) ? closeTagName.substring(colonPos + 1) : closeTagName;

    if (closeLocalName == localName) {
      String result = xml.substring(start, closeStart);
      result.trim();
      return result;
    }
    searchFrom = closeEnd + 1; // not a match - keep looking past it
  }
}

String findAttributeInTag(const String& tag, const String& attributeName) {
  String pattern = attributeName + "=";
  int searchFrom = 0;
  while (true) {
    int p = tag.indexOf(pattern, searchFrom);
    if (p < 0) return "";

    bool isBoundary = (p == 0) || tag[p - 1] == '<' || isspace((unsigned char)tag[p - 1]);
    if (isBoundary) {
      int valueStart = p + pattern.length();
      if (valueStart < (int)tag.length()) {
        char quote = tag[valueStart];
        if (quote == '"' || quote == '\'') {
          int q = tag.indexOf(quote, valueStart + 1);
          if (q > valueStart) return tag.substring(valueStart + 1, q);
        }
      }
    }
    searchFrom = p + 1;
  }
}

String findAttributeValue(const String& xml, const String& elementName, const String& attributeName) {
  // Anchored to a real tag name, not any substring (e.g. inside
  // "VideoProfiles" or an attribute).
  int element = xml.indexOf("<" + elementName);
  if (element < 0) element = xml.indexOf(":" + elementName);
  if (element < 0) return "";

  int end = xml.indexOf(">", element);
  if (end < 0) return "";
  return findAttributeInTag(xml.substring(element, end + 1), attributeName);
}

bool responseHasFault(const String& response) {
  if (response.indexOf("<s:Fault") >= 0 || response.indexOf(":Fault") >= 0) return true;

  // Also catch an unprefixed <Fault> (default-namespace envelope), requiring a
  // tag boundary after it so "FaultInfo" doesn't match.
  int p = response.indexOf("<Fault");
  while (p >= 0) {
    char next = (p + 6 < (int)response.length()) ? response[p + 6] : '\0';
    if (next == '>' || next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '/') return true;
    p = response.indexOf("<Fault", p + 1);
  }
  return false;
}

// Text of the first element with this local name at or after `from`, with or
// without a prefix or attributes; "" if there's none. *pos gets its position.
static String elementTextAfter(const String& xml, const char* localName, int from, int* pos = nullptr) {
  String prefixed = String(":") + localName;
  String bare = String("<") + localName;
  size_t nameLen = strlen(localName) + 1;
  for (int p = from; p >= 0;) {
    int a = xml.indexOf(prefixed, p);
    int b = xml.indexOf(bare, p);
    int hit = (a >= 0 && (b < 0 || a < b)) ? a : b;
    if (hit < 0) return "";
    char next = (hit + nameLen < xml.length()) ? xml[hit + nameLen] : '\0';
    if (next == '>' || next == ' ') {
      int open = xml.indexOf('>', hit);
      int close = xml.indexOf("</", open);
      if (open < 0 || close < 0) return "";
      if (pos) *pos = hit;
      return xml.substring(open + 1, close);
    }
    p = hit + 1;
  }
  return "";
}

String soapFaultSummary(const String& response) {
  int fault = response.indexOf(":Fault");
  if (fault < 0) fault = response.indexOf("<Fault");
  if (fault >= 0) {
    // SOAP 1.2: Code/Value, refined by Subcode/Value. SOAP 1.1: faultcode.
    String code;
    int subcode = -1;
    if (elementTextAfter(response, "Subcode", fault, &subcode).length() > 0) {
      code = elementTextAfter(response, "Value", subcode);
    }
    if (code.length() == 0) code = elementTextAfter(response, "Value", fault);
    if (code.length() == 0) code = elementTextAfter(response, "faultcode", fault);
    String reason = elementTextAfter(response, "Text", fault);
    if (reason.length() == 0) reason = elementTextAfter(response, "faultstring", fault);
    if (code.length() > 0 && reason.length() > 0) return code + ": " + reason;
    if (code.length() > 0 || reason.length() > 0) return code + reason;
  }
  return response.substring(0, 200);
}
