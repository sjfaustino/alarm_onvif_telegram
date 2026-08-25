#include "camera.h"
#include "onvif_soap.h"
#include "telegram.h"
#include <vector>

// Builds the envelope and posts it, honoring cfg.useWSSecurity: when true,
// auth rides in the WSSE header (the ONVIF-standard way); when false, the
// envelope carries no Security header and credentials go over HTTP Basic
// Auth instead, for cameras/stacks that expect that.
static String cameraSoapCall(const CameraConfig& cfg, const String& url, const String& to,
                              const String& action, const String& body) {
  String xml = soapEnvelope(action, body, to, cfg.user, cfg.pass,
                             cfg.includeReplyToAnonymous, cfg.useWSSecurity);
  const char* basicUser = cfg.useWSSecurity ? nullptr : cfg.user;
  const char* basicPass = cfg.useWSSecurity ? nullptr : cfg.pass;
  return soapPost(url, action, xml, basicUser, basicPass);
}

bool cameraDiscoverServices(const CameraConfig& cfg, CameraState& st) {
  Serial.printf("\n[%s] GetCapabilities\n", cfg.name);

  String action = "http://www.onvif.org/ver10/device/wsdl/GetCapabilities";
  String body = "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>";
  String response = cameraSoapCall(cfg, cfg.deviceServiceUrl, "", action, body);

  if (response.length() == 0 || responseHasFault(response) ||
      response.indexOf("GetCapabilitiesResponse") < 0) {
    Serial.printf("[%s] GetCapabilities FAILED\n", cfg.name);
    return false;
  }

  int eventsPos = response.indexOf("Events");
  if (eventsPos >= 0) {
    String discovered = findElementByLocalName(response, "XAddr", eventsPos);
    if (discovered.startsWith("http")) {
      st.eventServiceUrl = discovered;
      Serial.printf("[%s] Event service: %s\n", cfg.name, st.eventServiceUrl.c_str());
    }
  }
  if (st.eventServiceUrl.length() == 0) {
    Serial.printf("[%s] Event XAddr not found in GetCapabilities response.\n", cfg.name);
    return false;
  }

  int mediaPos = response.indexOf(":Media>");
  if (mediaPos >= 0) {
    String discoveredMedia = findElementByLocalName(response, "XAddr", mediaPos);
    if (discoveredMedia.startsWith("http")) {
      st.mediaServiceUrl = discoveredMedia;
      Serial.printf("[%s] Media service: %s\n", cfg.name, st.mediaServiceUrl.c_str());
    }
  }

  return true;
}

bool cameraGetEventServiceCapabilities(const CameraConfig& cfg, CameraState& st) {
  String action = "http://www.onvif.org/ver10/events/wsdl/EventPortType/GetServiceCapabilitiesRequest";
  String body = "<tev:GetServiceCapabilities/>";
  String response = cameraSoapCall(cfg, st.eventServiceUrl, st.eventServiceUrl, action, body);

  if (response.length() == 0 || responseHasFault(response) ||
      response.indexOf("GetServiceCapabilitiesResponse") < 0) {
    Serial.printf("[%s] GetServiceCapabilities FAILED\n", cfg.name);
    return false;
  }
  return true;
}

bool cameraGetEventProperties(const CameraConfig& cfg, CameraState& st) {
  String action = "http://www.onvif.org/ver10/events/wsdl/EventPortType/GetEventPropertiesRequest";
  String body = "<tev:GetEventProperties/>";
  String response = cameraSoapCall(cfg, st.eventServiceUrl, st.eventServiceUrl, action, body);

  if (response.length() == 0 || responseHasFault(response) ||
      response.indexOf("GetEventPropertiesResponse") < 0) {
    Serial.printf("[%s] GetEventProperties FAILED\n", cfg.name);
    return false;
  }
  return true;
}

// Restricted to actual <...Profiles ...> opening tags (not any "token=" in
// the document - GetProfiles responses also carry tokens on nested
// VideoEncoderConfiguration/VideoSourceConfiguration elements that aren't
// what we want here).
struct ProfileInfo { String token; String name; };

static std::vector<ProfileInfo> parseProfiles(const String& xml) {
  std::vector<ProfileInfo> profiles;
  int pos = 0;
  while (true) {
    int p = xml.indexOf("Profiles ", pos);
    if (p < 0) break;
    bool isOpeningTag = (p > 0) && (xml[p - 1] == ':' || xml[p - 1] == '<');
    if (!isOpeningTag) { pos = p + 9; continue; }

    int tagEnd = xml.indexOf(">", p);
    if (tagEnd < 0) break;
    String tag = xml.substring(p, tagEnd);

    String token;
    int tPos = tag.indexOf("token=\"");
    if (tPos >= 0) {
      int vs = tPos + 7;
      int ve = tag.indexOf("\"", vs);
      if (ve > vs) token = tag.substring(vs, ve);
    }

    String name;
    int namePos = xml.indexOf("Name>", tagEnd);
    if (namePos >= 0 && namePos - tagEnd < 300) {
      int cs = namePos + 5;
      int ce = xml.indexOf("</", cs);
      if (ce > cs) name = xml.substring(cs, ce);
    }

    if (token.length() > 0) profiles.push_back({token, name});
    pos = tagEnd + 1;
  }
  return profiles;
}

bool cameraFetchProfileAndSnapshotUri(const CameraConfig& cfg, CameraState& st) {
  Serial.printf("\n[%s] Resolving snapshot URI\n", cfg.name);

  if (cfg.snapshotUriOverride != nullptr) {
    st.snapshotUri = cfg.snapshotUriOverride;
    Serial.printf("[%s] Using configured snapshot override: %s\n", cfg.name, st.snapshotUri.c_str());
    return true;
  }

  if (st.mediaServiceUrl.length() == 0) {
    Serial.printf("[%s] No media service discovered, can't resolve snapshot URI.\n", cfg.name);
    return false;
  }

  String action = "http://www.onvif.org/ver10/media/wsdl/GetProfiles";
  String body = "<trt:GetProfiles/>";
  String response = cameraSoapCall(cfg, st.mediaServiceUrl, "", action, body);

  if (response.length() == 0 || responseHasFault(response)) {
    Serial.printf("[%s] GetProfiles FAILED\n", cfg.name);
    return false;
  }

  std::vector<ProfileInfo> profiles = parseProfiles(response);
  if (profiles.empty()) {
    Serial.printf("[%s] No profiles found in GetProfiles response.\n", cfg.name);
    return false;
  }

  ProfileInfo chosen = profiles[0];
  if (cfg.preferredProfileKeyword != nullptr) {
    String keyword = cfg.preferredProfileKeyword;
    keyword.toLowerCase();
    for (auto& p : profiles) {
      String lname = p.name;
      lname.toLowerCase();
      if (lname.indexOf(keyword) >= 0) { chosen = p; break; }
    }
  }
  st.profileToken = chosen.token;
  Serial.printf("[%s] Using profile '%s' (token=%s) out of %u found\n",
                cfg.name, chosen.name.c_str(), chosen.token.c_str(), (unsigned)profiles.size());

  String snapAction = "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri";
  String snapBody = "<trt:GetSnapshotUri><trt:ProfileToken>" + xmlEscape(st.profileToken) +
                     "</trt:ProfileToken></trt:GetSnapshotUri>";
  String snapResponse = cameraSoapCall(cfg, st.mediaServiceUrl, "", snapAction, snapBody);

  if (snapResponse.length() == 0 || responseHasFault(snapResponse)) {
    Serial.printf("[%s] GetSnapshotUri FAILED\n", cfg.name);
    return false;
  }

  st.snapshotUri = findElementByLocalName(snapResponse, "Uri");
  st.snapshotUri.trim();
  if (st.snapshotUri.length() == 0) {
    Serial.printf("[%s] Could not find snapshot URI in response.\n", cfg.name);
    return false;
  }

  Serial.printf("[%s] Snapshot URI: %s\n", cfg.name, st.snapshotUri.c_str());
  Serial.println("  ^ if this looks wrong (bad IP/port), that's the same GetSnapshotUri "
                  "quirk seen on the XM530 - you may need a snapshotUriOverride for this camera too.");
  return true;
}

bool cameraCreatePullPoint(const CameraConfig& cfg, CameraState& st) {
  Serial.printf("\n[%s] CreatePullPointSubscription (initTermTime=%d, replyToAnon=%d)\n",
                cfg.name, cfg.includeInitialTerminationTime, cfg.includeReplyToAnonymous);

  String action = "http://www.onvif.org/ver10/events/wsdl/EventPortType/CreatePullPointSubscriptionRequest";
  String body = "<tev:CreatePullPointSubscription>";
  if (cfg.includeInitialTerminationTime) {
    body += "<tev:InitialTerminationTime>PT5M</tev:InitialTerminationTime>";
  }
  body += "</tev:CreatePullPointSubscription>";

  String response = cameraSoapCall(cfg, st.eventServiceUrl, st.eventServiceUrl, action, body);

  if (response.length() == 0 || responseHasFault(response) ||
      response.indexOf("CreatePullPointSubscriptionResponse") < 0) {
    Serial.printf("[%s] CreatePullPointSubscription FAILED\n", cfg.name);
    return false;
  }

  String address = findElementByLocalName(response, "Address");
  address.trim();
  if (!address.startsWith("http")) {
    Serial.printf("[%s] No usable PullPoint address in response.\n", cfg.name);
    return false;
  }

  st.pullPointUrl = address;
  st.subscriptionActive = true;
  st.lastRenew = millis();
  st.lastPull = millis();
  Serial.printf("[%s] Subscription ACTIVE: %s\n", cfg.name, st.pullPointUrl.c_str());
  return true;
}

static void printEventState(const CameraConfig& cfg, const String& xml) {
  int p = xml.indexOf("Name=\"State\"");
  if (p < 0) p = xml.indexOf("Name=\"state\"");
  if (p < 0) return;

  int valuePos = xml.indexOf("Value=", p);
  if (valuePos < 0) return;
  int start = valuePos + strlen("Value=");
  if (start >= (int)xml.length()) return;
  char quote = xml[start];
  if (quote != '"' && quote != '\'') return;
  start++;
  int end = xml.indexOf(quote, start);
  if (end < 0) return;

  String state = xml.substring(start, end);
  state.trim();
  Serial.printf("[%s] State = %s\n", cfg.name, state.c_str());
}

static void parseEvents(const CameraConfig& cfg, CameraState& st, const String& xml) {
  bool anyTrue = xml.indexOf("Value=\"true\"") >= 0;
  if (!anyTrue && !VERBOSE_SOAP_LOG) return;

  bool motionAlarm = xml.indexOf("MotionAlarm") >= 0;
  bool cellMotion  = xml.indexOf("CellMotionDetector") >= 0;
  bool signalLoss  = xml.indexOf("SignalLoss") >= 0;
  bool tamper      = xml.indexOf("TamperDetector") >= 0;

  if (!motionAlarm && !cellMotion && !signalLoss && !tamper) return;

  if (motionAlarm) { Serial.printf("[%s] MOTION ALARM EVENT\n", cfg.name); printEventState(cfg, xml); }
  if (cellMotion)  { Serial.printf("[%s] CELL MOTION EVENT\n", cfg.name);  printEventState(cfg, xml); }
  if (signalLoss)  { Serial.printf("[%s] SIGNAL LOSS EVENT\n", cfg.name);  printEventState(cfg, xml); }
  if (tamper)      { Serial.printf("[%s] TAMPER EVENT\n", cfg.name);       printEventState(cfg, xml); }

  // Same simplification as the original: doesn't distinguish which topic was
  // true if several arrive in the same batch. Fine for a hobby alert bot;
  // if you need to alert on motion only, this needs per-NotificationMessage
  // parsing instead of whole-response substring search.
  if ((motionAlarm || cellMotion) && anyTrue) {
    triggerMotionAlert(cfg, st);
  }
}

bool cameraPullMessages(const CameraConfig& cfg, CameraState& st) {
  if (!st.subscriptionActive || st.pullPointUrl.length() == 0) return false;

  String action = "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest";
  // PT1S, not the original single-camera sketch's PT5S: with several cameras
  // round-robining, a 5s long-poll per camera stacks up badly across the
  // group. This trades a little poll efficiency for responsiveness.
  String body = "<tev:PullMessages><tev:Timeout>PT1S</tev:Timeout>"
                "<tev:MessageLimit>20</tev:MessageLimit></tev:PullMessages>";
  String response = cameraSoapCall(cfg, st.pullPointUrl, st.pullPointUrl, action, body);

  if (response.length() == 0) return false;

  if (response.indexOf("PullMessagesResponse") >= 0) {
    parseEvents(cfg, st, response);
    return true;
  }

  if (response.indexOf("ResourceUnknownFault") >= 0 || responseHasFault(response)) {
    Serial.printf("[%s] PullPoint gone, will resubscribe.\n", cfg.name);
    st.subscriptionActive = false;
    st.pullPointUrl = "";
  }
  return false;
}

bool cameraRenewSubscription(const CameraConfig& cfg, CameraState& st) {
  if (!st.subscriptionActive || st.pullPointUrl.length() == 0) return false;

  String action = "http://docs.oasis-open.org/wsn/bw-2/Renew";
  String body = "<wsnt:Renew><wsnt:TerminationTime>PT5M</wsnt:TerminationTime></wsnt:Renew>";
  String response = cameraSoapCall(cfg, st.pullPointUrl, st.pullPointUrl, action, body);

  if (response.indexOf("RenewResponse") >= 0) {
    st.lastRenew = millis();
    Serial.printf("[%s] Subscription renewed.\n", cfg.name);
    return true;
  }

  Serial.printf("[%s] Renew failed, will resubscribe.\n", cfg.name);
  st.subscriptionActive = false;
  st.pullPointUrl = "";
  return false;
}

bool cameraSetupSequence(const CameraConfig& cfg, CameraState& st) {
  if (!cameraDiscoverServices(cfg, st)) return false;
  if (!cameraFetchProfileAndSnapshotUri(cfg, st)) {
    Serial.printf("[%s] Snapshot URI not resolved - motion will still be detected "
                  "and logged, but photo alerts won't work until this is fixed.\n", cfg.name);
    // deliberately not returning false: detection/logging still has value
  }
  if (!cameraGetEventServiceCapabilities(cfg, st)) return false;
  if (!cameraGetEventProperties(cfg, st)) return false;
  if (!cameraCreatePullPoint(cfg, st)) return false;
  return true;
}
