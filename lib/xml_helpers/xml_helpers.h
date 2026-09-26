#pragma once
#include <Arduino.h>

// Dependency-free XML substring helpers, tested natively and re-exported by
// onvif_soap.h. Deliberately naive - not a real parser - but hardened for what
// real cameras send: either quote style, and closing tags that drop their
// namespace prefix.

String xmlEscape(const String& value);

String findElementByLocalName(const String& xml, const String& localName, int fromPosition = 0);
String findAttributeValue(const String& xml, const String& elementName, const String& attributeName);

// attributeName="value" (or '...') within one element's opening tag. Matches
// only at a name boundary ("id" doesn't match inside "profileid").
String findAttributeInTag(const String& tag, const String& attributeName);

bool responseHasFault(const String& response);
