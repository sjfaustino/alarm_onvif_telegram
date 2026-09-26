#pragma once
#include <Arduino.h>
#include "xml_helpers.h"

// Pure WS-Discovery message building and reply parsing, tested natively.

// Unfiltered Probe (no <d:Types>): some cameras silently ignore a Types value
// that doesn't match theirs exactly, while everything answers an unfiltered
// Probe. parseProbeMatch filters the replies instead.
String buildProbeMessage(const String& messageId);

// URL and best-effort name only - WS-Discovery carries no credentials.
struct DiscoveredCamera {
  String xaddr;     // device service URL, e.g. http://192.168.1.50/onvif/device_service
  String nameHint;  // best-effort friendly name parsed from Scopes, "" if none found
};

// False for non-ProbeMatch replies or ones without a usable XAddrs (NAS boxes,
// printers).
bool parseProbeMatch(const String& xml, DiscoveredCamera& out);
