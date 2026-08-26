#include "camera.h"
#include "onvif_soap.h"
#include "telegram.h"
#include <WiFi.h>
#include <vector>
#include <cstring>

// Looks up {user, pass} for cfg.name in CAMERA_SECRETS (secrets.h) by exact
// name match. Logs an error and returns false on no match, so a typo'd
// `name` in either config.h or secrets.h fails loudly at boot instead of
// silently sending the wrong (or a previous camera's) credentials.
bool resolveCameraCredentials(const CameraConfig& cfg, CameraState& st) {
  for (size_t i = 0; i < NUM_CAMERA_SECRETS; i++) {
    if (strcmp(CAMERA_SECRETS[i].name, cfg.name) == 0) {
      st.user = CAMERA_SECRETS[i].user;
      st.pass = CAMERA_SECRETS[i].pass;
      return true;
    }
  }
  Serial.printf("[%s] ERROR: no matching entry in CAMERA_SECRETS (secrets.h) - "
                "check that its `name` exactly matches this camera's name in "
                "config.h's CAMERAS[] (case-sensitive, no extra whitespace). "
                "%u secret entries checked.\n",
                cfg.name, (unsigned)NUM_CAMERA_SECRETS);
  return false;
}

// Builds the envelope and posts it, honoring cfg.useWSSecurity: when true,
// auth rides in the WSSE header (the ONVIF-standard way); when false, the
// envelope carries no Security header and credentials go over HTTP Basic
// Auth instead, for cameras/stacks that expect that. Credentials come from
// st.user/st.pass (resolved by name via resolveCameraCredentials at task
// startup), not from CameraConfig.
static String cameraSoapCall(const CameraConfig& cfg, const CameraState& st, const String& url,
                              const String& to, const String& action, const String& body) {
  String xml = soapEnvelope(action, body, to, st.user, st.pass,
                             cfg.includeReplyToAnonymous, cfg.useWSSecurity);
  const char* basicUser = cfg.useWSSecurity ? nullptr : st.user;
  const char* basicPass = cfg.useWSSecurity ? nullptr : st.pass;
  return soapPost(cfg.name, url, action, xml, basicUser, basicPass);
}

bool cameraDiscoverServices(const CameraConfig& cfg, CameraState& st) {
  Serial.printf("\n[%s] GetCapabilities\n", cfg.name);

  String action = "http://www.onvif.org/ver10/device/wsdl/GetCapabilities";
  String body = "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>";
  String response = cameraSoapCall(cfg, st, cfg.deviceServiceUrl, "", action, body);

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
  String response = cameraSoapCall(cfg, st, st.eventServiceUrl, st.eventServiceUrl, action, body);

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
  String response = cameraSoapCall(cfg, st, st.eventServiceUrl, st.eventServiceUrl, action, body);

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
    // {USER}/{PASS} let an override embed query-string auth (some Vstarcam
    // firmwares want ?loginuse=...&loginpas=... instead of HTTP Basic Auth)
    // without putting the actual credential in config.h, which is committed -
    // st.user/st.pass come from secrets.h (gitignored) via
    // resolveCameraCredentials, already resolved by the time this runs.
    st.snapshotUri.replace("{USER}", st.user);
    st.snapshotUri.replace("{PASS}", st.pass);
    String logUri = cfg.snapshotUriOverride; // log the un-substituted form - avoids echoing st.pass to serial
    logUri.replace("{PASS}", "***");
    Serial.printf("[%s] Using configured snapshot override: %s\n", cfg.name, logUri.c_str());
    return true;
  }

  if (st.mediaServiceUrl.length() == 0) {
    Serial.printf("[%s] No media service discovered, can't resolve snapshot URI.\n", cfg.name);
    return false;
  }

  String action = "http://www.onvif.org/ver10/media/wsdl/GetProfiles";
  String body = "<trt:GetProfiles/>";
  String response = cameraSoapCall(cfg, st, st.mediaServiceUrl, "", action, body);

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
  String snapResponse = cameraSoapCall(cfg, st, st.mediaServiceUrl, "", snapAction, snapBody);

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

  String response = cameraSoapCall(cfg, st, st.eventServiceUrl, st.eventServiceUrl, action, body);

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

// Scoped to the NotificationMessage block containing topicKeyword (from the
// keyword's own position up to that block's closing tag, or end of string)
// instead of searching the whole response from position 0 - otherwise, in a
// batch with several topics, this would report whichever topic's
// State/IsMotion happened to appear first in the XML, regardless of which
// one this log line is actually about. Recognizes both Name="State" (e.g.
// VideoSource/MotionAlarm) and Name="IsMotion" (CellMotionDetector/Motion) -
// different ONVIF stacks name the boolean differently for the same kind of
// event.
static void printEventState(const CameraConfig& cfg, const String& xml, const String& topicKeyword) {
  int topicPos = xml.indexOf(topicKeyword);
  if (topicPos < 0) return;
  int blockEnd = xml.indexOf("</wsnt:NotificationMessage>", topicPos);
  if (blockEnd < 0) blockEnd = xml.length();

  int p = xml.indexOf("Name=\"State\"", topicPos);
  if (p < 0 || p >= blockEnd) p = xml.indexOf("Name=\"state\"", topicPos);
  if (p < 0 || p >= blockEnd) p = xml.indexOf("Name=\"IsMotion\"", topicPos);
  if (p < 0 || p >= blockEnd) return;

  int valuePos = xml.indexOf("Value=", p);
  if (valuePos < 0 || valuePos >= blockEnd) return;
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

  if (motionAlarm) { Serial.printf("[%s] MOTION ALARM EVENT\n", cfg.name); printEventState(cfg, xml, "MotionAlarm"); }
  if (cellMotion)  { Serial.printf("[%s] CELL MOTION EVENT\n", cfg.name);  printEventState(cfg, xml, "CellMotionDetector"); }
  if (signalLoss)  { Serial.printf("[%s] SIGNAL LOSS EVENT\n", cfg.name);  printEventState(cfg, xml, "SignalLoss"); }
  if (tamper)      { Serial.printf("[%s] TAMPER EVENT\n", cfg.name);       printEventState(cfg, xml, "TamperDetector"); }

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
  // PT1S: a holdover from when this was single-camera-per-round-robin-slot
  // and a 5s long-poll per camera would stack up badly across the group.
  // Now that each camera has its own task (see cameraTaskFn), that
  // constraint is gone - PT1S still works fine and is left as-is, but PT5S
  // (fewer, longer polls) is worth trying if you want to reduce request
  // volume per camera.
  String body = "<tev:PullMessages><tev:Timeout>PT1S</tev:Timeout>"
                "<tev:MessageLimit>20</tev:MessageLimit></tev:PullMessages>";
  String response = cameraSoapCall(cfg, st, st.pullPointUrl, st.pullPointUrl, action, body);

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
  String response = cameraSoapCall(cfg, st, st.pullPointUrl, st.pullPointUrl, action, body);

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

// ============================================================
// Per-camera FreeRTOS task
//
// Replaces main.cpp's old round-robin loop() slot for this camera. Each
// enabled camera gets one of these, created once in setup() and pinned to
// core 1. Runs forever; never returns.
//
// This board turned out to be a genuine dual-core ESP32 (ESP32-D0WDQ6, per
// esptool chip-id), not the single-core S2 this was first written against -
// so with tasks pinned to core 1, this IS true parallel execution: camera
// A's slow SOAP call or Telegram TLS upload doesn't steal core 1 time from
// camera B's task the way it would on a single core, and neither competes
// with the WiFi/BT stack or the Arduino loopTask, which stay on core 0.
// ============================================================
void cameraTaskFn(void* pvParameters) {
  CameraTaskContext* ctx = static_cast<CameraTaskContext*>(pvParameters);
  const CameraConfig& cfg = *ctx->cfg;
  CameraState& st = *ctx->st;
  delete ctx; // context struct's job is done once we've unpacked it

  Serial.printf("[%s] Task started.\n", cfg.name);

  // Resolve credentials by name once, before anything else. A mismatch here
  // is a config typo, not a flaky network condition - retrying it forever
  // would just spam the log, so this camera's task exits instead. The
  // Telegram alert doesn't need this camera's credentials to send.
  if (!resolveCameraCredentials(cfg, st)) {
    Serial.printf("[%s] FATAL: no credentials resolved - task exiting, camera will NOT be monitored "
                  "until secrets.h is fixed and the board is reflashed.\n", cfg.name);
    sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F " + String(cfg.name) +
                         ": no matching entry in secrets.h's CAMERA_SECRETS - this camera is NOT being monitored.");
    vTaskDelete(nullptr);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!cameraSetupSequence(cfg, st)) {
      Serial.printf("[%s] Initial setup FAILED - will keep retrying.\n", cfg.name);
    }
  }

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      // main.cpp's loop() owns reconnecting; this task just waits and, once
      // back, treats itself as needing a fresh subscription (the old one
      // almost certainly timed out server-side during the outage anyway).
      if (st.subscriptionActive) {
        st.subscriptionActive = false;
        st.pullPointUrl = "";
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!st.subscriptionActive) {
      if (millis() - st.lastRetry >= RETRY_INTERVAL_MS) {
        // Jitter the retry timer (+/- up to 2s) so cameras that failed
        // together at boot or during a WiFi outage don't stay locked in
        // lockstep, all retrying (and all failing again, if the network's
        // still overwhelmed) on the same 10s tick forever.
        st.lastRetry = millis() - (unsigned long)random(0, 2001);
        Serial.printf("[%s] Retrying subscription...\n", cfg.name);
        if (st.eventServiceUrl.length() == 0) {
          cameraSetupSequence(cfg, st); // full rediscovery if we never got services
        } else if (cameraGetEventServiceCapabilities(cfg, st) && cameraCreatePullPoint(cfg, st)) {
          Serial.printf("[%s] Subscription recovered.\n", cfg.name);
        }
      }
    } else {
      if (millis() - st.lastPull >= PULL_INTERVAL_MS) {
        st.lastPull = millis();
        cameraPullMessages(cfg, st);
      }
      if (millis() - st.lastRenew >= (SUBSCRIPTION_LIFETIME_MS - RENEW_MARGIN_MS)) {
        cameraRenewSubscription(cfg, st);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
