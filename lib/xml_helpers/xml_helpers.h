#pragma once
#include <Arduino.h>

// Small, dependency-free XML substring helpers shared by onvif_soap.cpp's
// SOAP envelope building and camera.cpp's response parsing. Split out into
// their own module (no WiFi/HTTPClient/mbedtls includes) purely so they can
// be unit-tested natively (see test/test_xml_helpers) without dragging in
// anything ESP32-specific - onvif_soap.h re-exports these under its own
// public API so existing callers don't need to change anything.
//
// These are deliberately naive substring search, not a real XML parser -
// see the project README's reliability notes for the tradeoffs that come
// with that (a namespace variant, CDATA, or a self-closing tag can all
// confuse it). Good enough for the specific ONVIF responses this project
// has been tuned against; the tests exist to catch a regression in that
// behavior, not to claim it's a general-purpose XML parser.

String xmlEscape(const String& value);

String findElementByLocalName(const String& xml, const String& localName, int fromPosition = 0);
String findAttributeValue(const String& xml, const String& elementName, const String& attributeName);
bool   responseHasFault(const String& response);
