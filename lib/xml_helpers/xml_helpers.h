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
// with that (a genuinely nested/mixed-content document, or arbitrary
// namespace prefixes, can still confuse it). Good enough for the specific
// ONVIF responses this project has been tuned against; the tests exist to
// catch a regression in that behavior, not to claim it's a general-purpose
// XML parser. findElementByLocalName and findAttributeValue/
// findAttributeInTag are hardened against the specific failure modes real
// cameras in this project's fleet have hit - inconsistent attribute quote
// style, and a closing tag that drops the namespace prefix its own opening
// tag used - without changing behavior for anything that already worked.

String xmlEscape(const String& value);

String findElementByLocalName(const String& xml, const String& localName, int fromPosition = 0);
String findAttributeValue(const String& xml, const String& elementName, const String& attributeName);

// Finds attributeName="value" or attributeName='value' within `tag` (a
// single element's opening tag, e.g. "<trt:Profiles token=\"P1\">" or just
// "trt:Profiles token='P1'") - tolerates either quote style, and requires
// the match to start at a real attribute-name boundary (preceded by
// whitespace or '<'), not partway through a longer attribute name that
// happens to end the same way (e.g. finding "id" inside "profileid").
// Exposed separately from findAttributeValue so a caller that's already
// isolated one element's tag substring (like camera.cpp's parseProfiles,
// which loops over several same-named elements by hand) can search it
// directly instead of re-finding the element from scratch.
String findAttributeInTag(const String& tag, const String& attributeName);

bool responseHasFault(const String& response);
