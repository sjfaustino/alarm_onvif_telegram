#pragma once
#include <Arduino.h>
#include "xml_helpers.h"

// Pure WS-Discovery (the multicast protocol ONVIF cameras use to announce
// themselves on the local network) message building and reply parsing,
// split out of webserver_cameras.cpp's UDP send/receive glue so it can be
// unit-tested natively (test/test_onvif_discovery) without WiFiUdp, which
// only exists on-device.

// Builds a WS-Discovery Probe envelope (SOAP 1.2, the WS-Discovery 2005
// dialect every ONVIF device's discovery listener speaks), addressed with
// the given messageId (pass makeUUID()'s output - onvif_soap.h). No
// <d:Types> filter is included deliberately: several real cameras only
// match an exact, case-sensitive Types value and silently drop a Probe
// that guesses wrong, where an unfiltered Probe (matches every device on
// the segment, per spec) is answered by everything - the caller is
// expected to sanity-check each reply's XAddrs/Scopes look ONVIF-ish
// (parseProbeMatch below requires a usable XAddrs) rather than trusting
// Types filtering to have done that job.
String buildProbeMessage(const String& messageId);

// One camera's ProbeMatch reply, condensed to what the Add-camera flow
// actually needs. Discovery only ever hands back a URL and a best-effort
// name - never credentials (WS-Discovery doesn't carry them), so the Add
// form still needs a username/password typed in by hand.
struct DiscoveredCamera {
  String xaddr;     // device service URL, e.g. http://192.168.1.50/onvif/device_service
  String nameHint;  // best-effort friendly name parsed from Scopes, "" if none found
};

// Parses one ProbeMatch UDP reply (one camera's whole response datagram).
// Returns false (out untouched) if this doesn't look like a ProbeMatch, or
// it has no usable XAddrs entry - happens for noise from non-ONVIF devices
// that also answer an unfiltered WS-Discovery Probe (some NAS boxes,
// printers), which this project has no use listing as an addable camera.
bool parseProbeMatch(const String& xml, DiscoveredCamera& out);
