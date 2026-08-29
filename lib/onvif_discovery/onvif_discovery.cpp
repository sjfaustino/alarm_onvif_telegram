#include "onvif_discovery.h"
#include <cstring>

String buildProbeMessage(const String& messageId) {
  String xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  xml += "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
         "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
         "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">";
  xml += "<e:Header>";
  xml += "<w:MessageID>" + messageId + "</w:MessageID>";
  xml += "<w:To e:mustUnderstand=\"1\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>";
  xml += "<w:Action e:mustUnderstand=\"1\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>";
  xml += "</e:Header>";
  xml += "<e:Body><d:Probe/></e:Body>";
  xml += "</e:Envelope>";
  return xml;
}

// Extracts the value of the first "prefix" scope token (e.g.
// "onvif://www.onvif.org/name/") out of a space-separated Scopes string,
// stopping at the next space or the end of the string. "" if the prefix
// isn't present at all.
static String extractScopeValue(const String& scopes, const char* prefix) {
  int p = scopes.indexOf(prefix);
  if (p < 0) return "";
  int start = p + (int)strlen(prefix);
  int end = scopes.indexOf(' ', start);
  String raw = (end < 0) ? scopes.substring(start) : scopes.substring(start, end);
  raw.trim();
  // Scopes are URI tokens - '%20' is the common escape a manufacturer's
  // discovery stack uses for a space in a name (e.g. "Front%20Door") - not
  // a full percent-decoder, just the one escape actually seen in practice,
  // good enough for a best-effort hint string the user can still edit.
  raw.replace("%20", " ");
  return raw;
}

bool parseProbeMatch(const String& xml, DiscoveredCamera& out) {
  if (xml.indexOf("ProbeMatch") < 0) return false;

  String xaddrs = findElementByLocalName(xml, "XAddrs");
  if (xaddrs.length() == 0) return false;

  // XAddrs is a space-separated list (a device can advertise more than one
  // - IPv4/IPv6, http/https variants). Prefer the first http:// entry;
  // fall back to whatever token came first if none of them are (an
  // https-only or bare-address stack).
  String chosen;
  int pos = 0;
  while (pos < (int)xaddrs.length()) {
    int spacePos = xaddrs.indexOf(' ', pos);
    String token = (spacePos < 0) ? xaddrs.substring(pos) : xaddrs.substring(pos, spacePos);
    token.trim();
    if (token.length() > 0) {
      if (chosen.length() == 0) chosen = token;
      if (token.startsWith("http://")) { chosen = token; break; }
    }
    if (spacePos < 0) break;
    pos = spacePos + 1;
  }
  if (chosen.length() == 0) return false;
  out.xaddr = chosen;

  String scopes = findElementByLocalName(xml, "Scopes");
  out.nameHint = extractScopeValue(scopes, "onvif://www.onvif.org/name/");
  if (out.nameHint.length() == 0) {
    out.nameHint = extractScopeValue(scopes, "onvif://www.onvif.org/hardware/");
  }
  return true;
}
