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

  // Unlike the unprefixed branch above (which searches for the exact
  // closing tag string), a namespace prefix on the opening tag doesn't
  // guarantee the closing tag repeats the same prefix - some ONVIF stacks
  // are inconsistent about that. Scan forward through *every* "</...>" from
  // here, comparing each one's own local name (the part after its own
  // last ':', if any) against localName, so a nested child element's
  // closing tag that happens to come first doesn't get mistaken for this
  // element's own end.
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
  // Anchored the same two ways findElementByLocalName is (a bare "<name"
  // opening tag, or a ":name" namespaced one) rather than a plain
  // substring search for elementName anywhere in the document, which
  // could otherwise match inside an unrelated longer tag/attribute name
  // that just happens to contain it (e.g. "Profiles" inside a
  // "VideoProfiles"-named element, or inside some attribute's value).
  int element = xml.indexOf("<" + elementName);
  if (element < 0) element = xml.indexOf(":" + elementName);
  if (element < 0) return "";

  int end = xml.indexOf(">", element);
  if (end < 0) return "";
  return findAttributeInTag(xml.substring(element, end + 1), attributeName);
}

bool responseHasFault(const String& response) {
  return response.indexOf("<s:Fault") >= 0 || response.indexOf(":Fault") >= 0;
}
