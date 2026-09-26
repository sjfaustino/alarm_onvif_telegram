#include "camera.h"
#include "onvif_soap.h"
#include "telegram.h"
#include "telegram_i18n.h"
#include "backoff.h"
#include "camera_parse.h"
#include "event_log_store.h"
#include "config.h" // CAMERA_SNAPSHOT_DIMENSION_MAX
#include <WiFi.h>
#include <vector>
#include <cstring>
#include <algorithm>

void cameraStateInit(CameraState& st) {
  if (!st.stateMutex) st.stateMutex = xSemaphoreCreateMutex();
}

void requestLiveConfigReload(CameraState& st, const CameraConfig& newConfig) {
  CameraConfig* copy = new CameraConfig(newConfig);
  CameraConfig* old = nullptr;
  {
    CameraStateLock lock(st);
    old = st.pendingConfig; // whatever wasn't applied yet loses to this newer edit
    st.pendingConfig = copy;
  }
  delete old; // safe outside the lock - no longer reachable via st.pendingConfig once swapped above
}

void requestCameraStop(CameraState& st) {
  CameraStateLock lock(st);
  st.stopRequested = true;
}

// Applies a staged pendingConfig to cfg/st; returns whether one was applied
// (the loop then resets subscription state). Also used at startup: a camera
// enabled live gets its real config this way, because only the owning task may
// write its CameraConfig (see requestLiveConfigReload).
static bool applyPendingConfigIfAny(CameraConfig& cfg, CameraState& st) {
  CameraConfig* pending = nullptr;
  { CameraStateLock lock(st); pending = st.pendingConfig; st.pendingConfig = nullptr; }
  if (!pending) return false;

  cfg = *pending; // safe: this task is the sole writer of its own cfg - see requestLiveConfigReload's comment
  delete pending;

  // Always re-point, valid or not: the assignment above may have freed the old
  // buffers.
  { CameraStateLock lock(st); st.user = cfg.user.c_str(); st.pass = cfg.pass.c_str(); }
  return true;
}

bool resolveCameraCredentials(const CameraConfig& cfg, CameraState& st) {
  if (cfg.user.length() == 0 || cfg.pass.length() == 0) {
    Serial.printf("[%s] ERROR: no username/password set for this camera - add them via the web UI.\n",
                  cfg.name.c_str());
    return false;
  }
  CameraStateLock lock(st);
  st.user = cfg.user.c_str();
  st.pass = cfg.pass.c_str();
  return true;
}

// Every SOAP call goes through here, honouring useWSSecurity. Any non-empty
// response (even a fault) refreshes lastContactMs, which separates offline
// from failing. Locked because pushCameraSnapshot also adjusts it from other
// tasks.
static String cameraSoapCall(const CameraConfig& cfg, CameraState& st, const String& url,
                              const String& to, const String& action, const String& body) {
  const char* user; const char* pass;
  { CameraStateLock lock(st); user = st.user; pass = st.pass; }
  String xml = soapEnvelope(action, body, to, user, pass,
                             cfg.includeReplyToAnonymous, cfg.useWSSecurity);
  const char* basicUser = cfg.useWSSecurity ? nullptr : user;
  const char* basicPass = cfg.useWSSecurity ? nullptr : pass;
  String response = soapPost(cfg.name.c_str(), url, action, xml, basicUser, basicPass);
  if (response.length() > 0) { CameraStateLock lock(st); st.lastContactMs = millis(); }
  return response;
}

bool cameraDiscoverServices(const CameraConfig& cfg, CameraState& st) {
  Serial.printf("\n[%s] GetCapabilities\n", cfg.name.c_str());

  String action = "http://www.onvif.org/ver10/device/wsdl/GetCapabilities";
  String body = "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>";
  String response = cameraSoapCall(cfg, st, cfg.deviceServiceUrl, "", action, body);

  if (response.length() == 0 || responseHasFault(response) ||
      response.indexOf("GetCapabilitiesResponse") < 0) {
    Serial.printf("[%s] GetCapabilities FAILED\n", cfg.name.c_str());
    return false;
  }

  int eventsPos = response.indexOf("Events");
  if (eventsPos >= 0) {
    String discovered = findElementByLocalName(response, "XAddr", eventsPos);
    if (discovered.startsWith("http")) {
      st.eventServiceUrl = discovered;
      Serial.printf("[%s] Event service: %s\n", cfg.name.c_str(), st.eventServiceUrl.c_str());
    }
  }
  if (st.eventServiceUrl.length() == 0) {
    Serial.printf("[%s] Event XAddr not found in GetCapabilities response.\n", cfg.name.c_str());
    return false;
  }

  int mediaPos = response.indexOf(":Media>");
  if (mediaPos >= 0) {
    String discoveredMedia = findElementByLocalName(response, "XAddr", mediaPos);
    if (discoveredMedia.startsWith("http")) {
      st.mediaServiceUrl = discoveredMedia;
      Serial.printf("[%s] Media service: %s\n", cfg.name.c_str(), st.mediaServiceUrl.c_str());
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
    Serial.printf("[%s] GetServiceCapabilities FAILED\n", cfg.name.c_str());
    return false;
  }
  return true;
}

// The topic keywords acted on anywhere; one list so advertised-topic scans and
// live-event matching can't drift.
static const char* const kKnownEventTopics[] = {
    "PeopleDetect", "VehicleDetect", "DogCatDetect", "MotionAlarm", "CellMotionDetector",
    "TamperDetector", "SignalLoss",
};

// Which known keywords appear in the response - a substring scan, the same
// matching live events use (topic paths are vendor-prefixed and nested, so no
// tree parse). "" if none.
static String scanKnownEventTopics(const String& response) {
  String found;
  for (const char* topic : kKnownEventTopics) {
    if (response.indexOf(topic) < 0) continue;
    if (found.length() > 0) found += ", ";
    found += topic;
  }
  return found;
}

// Local names of elements marked wstop:topic="true" (real subscribable
// topics), in first-seen order. Reads back only to each marker's own tag - no
// tree parse.
static std::vector<String> findAllTopicElementNames(const String& xml) {
  std::vector<String> names;
  int searchFrom = 0;
  while (true) {
    int viaDouble = xml.indexOf("topic=\"true\"", searchFrom);
    int viaSingle = xml.indexOf("topic='true'", searchFrom);
    int markerPos = (viaDouble < 0) ? viaSingle : (viaSingle < 0 ? viaDouble : std::min(viaDouble, viaSingle));
    if (markerPos < 0) break;

    int tagStart = xml.lastIndexOf('<', markerPos);
    if (tagStart < 0) { searchFrom = markerPos + 1; continue; }
    int nameStart = tagStart + 1;
    int nameEnd = nameStart;
    while (nameEnd < (int)xml.length()) {
      char c = xml[nameEnd];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == '/') break;
      nameEnd++;
    }
    String fullName = xml.substring(nameStart, nameEnd);
    int colon = fullName.indexOf(':');
    String localName = colon >= 0 ? fullName.substring(colon + 1) : fullName;

    if (localName.length() > 0) {
      bool alreadySeen = false;
      for (auto& n : names) {
        if (n == localName) { alreadySeen = true; break; }
      }
      if (!alreadySeen) names.push_back(localName);
    }
    searchFrom = markerPos + 1;
  }
  return names;
}

bool cameraGetEventProperties(const CameraConfig& cfg, CameraState& st, String* outTopics,
                               String* outUnusedTopics) {
  String action = "http://www.onvif.org/ver10/events/wsdl/EventPortType/GetEventPropertiesRequest";
  String body = "<tev:GetEventProperties/>";
  String response = cameraSoapCall(cfg, st, st.eventServiceUrl, st.eventServiceUrl, action, body);

  if (response.length() == 0 || responseHasFault(response) ||
      response.indexOf("GetEventPropertiesResponse") < 0) {
    Serial.printf("[%s] GetEventProperties FAILED\n", cfg.name.c_str());
    return false;
  }
  if (outTopics) *outTopics = scanKnownEventTopics(response);

  // Topics we don't act on yet, shown on the dashboard and logged once so new
  // capabilities get noticed.
  std::vector<String> allTopicNames = findAllTopicElementNames(response);
  String unusedTopics;
  for (auto& name : allTopicNames) {
    bool known = false;
    for (const char* k : kKnownEventTopics) {
      if (name == k) { known = true; break; }
    }
    if (known) continue;
    if (unusedTopics.length() > 0) unusedTopics += ", ";
    unusedTopics += name;
  }
  if (outUnusedTopics) *outUnusedTopics = unusedTopics;
  if (unusedTopics.length() > 0) {
    logEvent(cfg.name + ": event schema also advertises " + unusedTopics +
             " - not used by this firmware yet");
  }
  return true;
}

// Clamped at use; 0 means unset.
static uint16_t safeSnapshotDimension(uint16_t value) {
  if (value == 0) return 0;
  if (value > CAMERA_SNAPSHOT_DIMENSION_MAX) return CAMERA_SNAPSHOT_DIMENSION_MAX;
  return value;
}

bool cameraFetchProfileAndSnapshotUri(const CameraConfig& cfg, CameraState& st) {
  Serial.printf("\n[%s] Resolving snapshot URI\n", cfg.name.c_str());

  if (cfg.snapshotUriOverride.length() > 0) {
    // {USER}/{PASS} allow query-string auth (e.g. Vstarcam's
    // loginuse/loginpas) without credentials in a file; {WIDTH}/{HEIGHT}
    // request a size. No-op if the override doesn't use them.
    String resolved = cfg.snapshotUriOverride;
    String user;
    {
      CameraStateLock lock(st);
      user = st.user;
      resolved.replace("{USER}", st.user);
      resolved.replace("{PASS}", st.pass);
      resolved.replace("{WIDTH}", String(safeSnapshotDimension(cfg.snapshotMaxWidth)));
      resolved.replace("{HEIGHT}", String(safeSnapshotDimension(cfg.snapshotMaxHeight)));
      st.snapshotUri = resolved;
    }
    // Log with the password masked.
    String logUri = cfg.snapshotUriOverride;
    logUri.replace("{USER}", user);
    logUri.replace("{PASS}", "***");
    logUri.replace("{WIDTH}", String(safeSnapshotDimension(cfg.snapshotMaxWidth)));
    logUri.replace("{HEIGHT}", String(safeSnapshotDimension(cfg.snapshotMaxHeight)));
    Serial.printf("[%s] Using configured snapshot override: %s\n", cfg.name.c_str(), logUri.c_str());
    return true;
  }

  if (st.mediaServiceUrl.length() == 0) {
    Serial.printf("[%s] No media service discovered, can't resolve snapshot URI.\n", cfg.name.c_str());
    return false;
  }

  String action = "http://www.onvif.org/ver10/media/wsdl/GetProfiles";
  String body = "<trt:GetProfiles/>";
  String response = cameraSoapCall(cfg, st, st.mediaServiceUrl, "", action, body);

  if (response.length() == 0 || responseHasFault(response)) {
    Serial.printf("[%s] GetProfiles FAILED\n", cfg.name.c_str());
    return false;
  }

  std::vector<ProfileInfo> profiles = parseProfiles(response);
  if (profiles.empty()) {
    Serial.printf("[%s] No profiles found in GetProfiles response.\n", cfg.name.c_str());
    return false;
  }

  ProfileInfo chosen = profiles[0];
  if (cfg.preferredProfileKeyword.length() > 0) {
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
                cfg.name.c_str(), chosen.name.c_str(), chosen.token.c_str(), (unsigned)profiles.size());

  String snapAction = "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri";
  String snapBody = "<trt:GetSnapshotUri><trt:ProfileToken>" + xmlEscape(st.profileToken) +
                     "</trt:ProfileToken></trt:GetSnapshotUri>";
  String snapResponse = cameraSoapCall(cfg, st, st.mediaServiceUrl, "", snapAction, snapBody);

  if (snapResponse.length() == 0 || responseHasFault(snapResponse)) {
    Serial.printf("[%s] GetSnapshotUri FAILED\n", cfg.name.c_str());
    return false;
  }

  String resolvedUri = findElementByLocalName(snapResponse, "Uri");
  resolvedUri.trim();
  if (resolvedUri.length() == 0) {
    Serial.printf("[%s] Could not find snapshot URI in response.\n", cfg.name.c_str());
    return false;
  }
  { CameraStateLock lock(st); st.snapshotUri = resolvedUri; }

  Serial.printf("[%s] Snapshot URI: %s\n", cfg.name.c_str(), resolvedUri.c_str());
  Serial.println("  ^ if this looks wrong (bad IP/port), that's the same GetSnapshotUri "
                  "quirk seen on the XM530 - you may need a snapshotUriOverride for this camera too.");

  // Best-effort RTSP URI, reusing the chosen profile.
  String streamAction = "http://www.onvif.org/ver10/media/wsdl/GetStreamUri";
  String streamBody = "<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
                       "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup>"
                       "<trt:ProfileToken>" + xmlEscape(st.profileToken) + "</trt:ProfileToken></trt:GetStreamUri>";
  String streamResponse = cameraSoapCall(cfg, st, st.mediaServiceUrl, "", streamAction, streamBody);
  if (streamResponse.length() == 0 || responseHasFault(streamResponse)) {
    Serial.printf("[%s] GetStreamUri FAILED - no RTSP link will be shown on the dashboard for this "
                  "camera, motion detection/snapshot alerts are unaffected.\n", cfg.name.c_str());
    return true;
  }
  String resolvedStreamUri = findElementByLocalName(streamResponse, "Uri");
  resolvedStreamUri.trim();
  if (resolvedStreamUri.length() > 0) {
    CameraStateLock lock(st);
    st.streamUri = resolvedStreamUri;
    Serial.printf("[%s] Stream URI: %s\n", cfg.name.c_str(), resolvedStreamUri.c_str());
  }

  // Best-effort MJPEG URI, only if some profile is JPEG (rare).
  ProfileInfo* jpegProfile = nullptr;
  for (auto& p : profiles) {
    if (p.encoding.equalsIgnoreCase("JPEG")) { jpegProfile = &p; break; }
  }
  if (jpegProfile) {
    // HTTP transport (an <img> can show MJPEG-over-HTTP). Often unsupported,
    // so failure is only traced.
    String mjpegAction = "http://www.onvif.org/ver10/media/wsdl/GetStreamUri";
    String mjpegBody = "<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
                        "<tt:Transport><tt:Protocol>HTTP</tt:Protocol></tt:Transport></trt:StreamSetup>"
                        "<trt:ProfileToken>" + xmlEscape(jpegProfile->token) + "</trt:ProfileToken></trt:GetStreamUri>";
    String mjpegResponse = cameraSoapCall(cfg, st, st.mediaServiceUrl, "", mjpegAction, mjpegBody);
    if (mjpegResponse.length() > 0 && !responseHasFault(mjpegResponse)) {
      String resolvedMjpegUri = findElementByLocalName(mjpegResponse, "Uri");
      resolvedMjpegUri.trim();
      if (resolvedMjpegUri.length() > 0) {
        CameraStateLock lock(st);
        st.mjpegUri = resolvedMjpegUri;
        Serial.printf("[%s] MJPEG preview URI (profile '%s'): %s\n",
                      cfg.name.c_str(), jpegProfile->name.c_str(), resolvedMjpegUri.c_str());
      }
    } else {
      Serial.printf("[%s] Camera has a JPEG-encoded profile ('%s') but doesn't support HTTP transport "
                    "for it - no live preview will be shown on the dashboard for this camera.\n",
                    cfg.name.c_str(), jpegProfile->name.c_str());
    }
  }
  return true;
}

bool cameraCreatePullPoint(const CameraConfig& cfg, CameraState& st) {
  Serial.printf("\n[%s] CreatePullPointSubscription (initTermTime=%d, replyToAnon=%d)\n",
                cfg.name.c_str(), cfg.includeInitialTerminationTime, cfg.includeReplyToAnonymous);

  String action = "http://www.onvif.org/ver10/events/wsdl/EventPortType/CreatePullPointSubscriptionRequest";
  String body = "<tev:CreatePullPointSubscription>";
  if (cfg.includeInitialTerminationTime) {
    body += "<tev:InitialTerminationTime>PT5M</tev:InitialTerminationTime>";
  }
  body += "</tev:CreatePullPointSubscription>";

  String response = cameraSoapCall(cfg, st, st.eventServiceUrl, st.eventServiceUrl, action, body);

  if (response.length() == 0 || responseHasFault(response) ||
      response.indexOf("CreatePullPointSubscriptionResponse") < 0) {
    Serial.printf("[%s] CreatePullPointSubscription FAILED\n", cfg.name.c_str());
    return false;
  }

  String address = findElementByLocalName(response, "Address");
  address.trim();
  if (!address.startsWith("http")) {
    Serial.printf("[%s] No usable PullPoint address in response.\n", cfg.name.c_str());
    return false;
  }

  st.pullPointUrl = address;
  { CameraStateLock lock(st); st.subscriptionActive = true; }
  st.lastRenew = millis();
  st.lastPull = millis();
  Serial.printf("[%s] Subscription ACTIVE: %s\n", cfg.name.c_str(), st.pullPointUrl.c_str());
  return true;
}

static void printEventState(const CameraConfig& cfg, const String& xml, const String& topicKeyword) {
  String state = extractEventStateValue(xml, topicKeyword);
  if (state.length() > 0) Serial.printf("[%s] State = %s\n", cfg.name.c_str(), state.c_str());
}

static void parseEvents(const CameraConfig& cfg, CameraState& st, const String& xml) {
  CameraEventClassification ev = classifyCameraEvent(xml);
  if (!ev.anyTrue && !VERBOSE_SOAP_LOG) return;
  if (!ev.motionAlarm && !ev.cellMotion && !ev.peopleDetect && !ev.vehicleDetect && !ev.dogCatDetect &&
      !ev.signalLoss && !ev.tamper) {
    // A real notification with no known topic: log it so support can be added.
    if (xml.indexOf("NotificationMessage") >= 0) {
      String topic = firstTopic(xml);
      Serial.printf("[%s] UNRECOGNIZED EVENT - topic: %s (enable VERBOSE_SOAP_LOG to see the full response)\n",
                    cfg.name.c_str(), topic.length() > 0 ? topic.c_str() : "(no Topic element found)");
      logEvent(cfg.name + ": unrecognized ONVIF event" + (topic.length() > 0 ? " (" + topic + ")" : ""));
    }
    return;
  }

  if (ev.motionAlarm)  { Serial.printf("[%s] MOTION ALARM EVENT\n", cfg.name.c_str());  printEventState(cfg, xml, "MotionAlarm"); }
  if (ev.cellMotion)   { Serial.printf("[%s] CELL MOTION EVENT\n", cfg.name.c_str());   printEventState(cfg, xml, "CellMotionDetector"); }
  if (ev.peopleDetect)  { Serial.printf("[%s] PEOPLE DETECT EVENT\n", cfg.name.c_str());  printEventState(cfg, xml, "PeopleDetect"); }
  if (ev.vehicleDetect) { Serial.printf("[%s] VEHICLE DETECT EVENT\n", cfg.name.c_str()); printEventState(cfg, xml, "VehicleDetect"); }
  if (ev.dogCatDetect) { Serial.printf("[%s] DOG/CAT DETECT EVENT\n", cfg.name.c_str());  printEventState(cfg, xml, "DogCatDetect"); }
  if (ev.signalLoss)    { Serial.printf("[%s] SIGNAL LOSS EVENT\n", cfg.name.c_str());    printEventState(cfg, xml, "SignalLoss"); }
  if (ev.tamper)       { Serial.printf("[%s] TAMPER EVENT\n", cfg.name.c_str());        printEventState(cfg, xml, "TamperDetector"); }

  // Every check below uses the topic's own state, not the body-wide flags.
  if (motionEventFired(xml, ev)) {
    st.lastMotionMs = millis(); // real motion signal, independent of mute/cooldown/quiet hours - see checkMotionWatchdog
    // Checked separately: one batch can report person and vehicle together,
    // and each has its own opt-out. Collapsing to one kind first once let a
    // muted person alert swallow a wanted vehicle alert.
    bool personDetected = ev.peopleDetect && topicReportedTrue(xml, "PeopleDetect");
    bool vehicleDetected = ev.vehicleDetect && topicReportedTrue(xml, "VehicleDetect");
    bool plainMotionDetected = (ev.motionAlarm && topicReportedTrue(xml, "MotionAlarm")) ||
                               (ev.cellMotion && topicReportedTrue(xml, "CellMotionDetector"));

    bool personWantsAlert = personDetected && cfg.personAlertsEnabled;
    bool vehicleWantsAlert = vehicleDetected && cfg.vehicleAlertsEnabled;

    if (personWantsAlert || vehicleWantsAlert || plainMotionDetected) {
      // Person wins the caption when both qualify; plain motion is Generic.
      MotionDetectionKind kind = personWantsAlert ? MotionDetectionKind::Person
                                 : vehicleWantsAlert ? MotionDetectionKind::Vehicle
                                                      : MotionDetectionKind::Generic;
      triggerMotionAlert(cfg, st, false, kind);
    } else {
      // All fired topics were muted types; log each.
      if (personDetected) logEvent(cfg.name + ": person detected (alerts off)");
      if (vehicleDetected) logEvent(cfg.name + ": vehicle detected (alerts off)");
    }
  } else if (ev.dogCatDetect && topicReportedTrue(xml, "DogCatDetect")) {
    // Pet-only event (opt-in per camera). Always counts as motion for the
    // watchdog - it proves detection works.
    st.lastMotionMs = millis();
    if (cfg.petAlertsEnabled) {
      triggerMotionAlert(cfg, st, true);
    } else {
      logEvent(cfg.name + ": pet detected (alerts off)");
    }
  }
  if (ev.tamper && topicReportedTrue(xml, "TamperDetector")) {
    triggerTamperAlert(cfg, st);
  }
  if (ev.signalLoss && topicReportedTrue(xml, "SignalLoss")) {
    triggerSignalLossAlert(cfg, st);
  }
}

bool cameraPullMessages(const CameraConfig& cfg, CameraState& st) {
  if (!st.subscriptionActive || st.pullPointUrl.length() == 0) return false; // same-task read, no lock needed

  String action = "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest";
  // PT1S long-poll. PT5S would cut request volume at the cost of latency.
  String body = "<tev:PullMessages><tev:Timeout>PT1S</tev:Timeout>"
                "<tev:MessageLimit>20</tev:MessageLimit></tev:PullMessages>";
  String response = cameraSoapCall(cfg, st, st.pullPointUrl, st.pullPointUrl, action, body);

  if (response.length() == 0) return false;

  if (response.indexOf("PullMessagesResponse") >= 0) {
    st.pullAmbiguousStreak = 0;
    parseEvents(cfg, st, response);
    return true;
  }

  if (response.indexOf("ResourceUnknownFault") >= 0 || responseHasFault(response)) {
    Serial.printf("[%s] PullPoint gone, will resubscribe.\n", cfg.name.c_str());
    { CameraStateLock lock(st); st.subscriptionActive = false; }
    st.pullPointUrl = "";
    st.pullAmbiguousStreak = 0;
    return false;
  }

  // Unrecognized response (neither success nor fault). Resubscribe after
  // PULL_MESSAGES_AMBIGUOUS_LIMIT in a row instead of polling a dead pull
  // point forever.
  if (++st.pullAmbiguousStreak >= PULL_MESSAGES_AMBIGUOUS_LIMIT) {
    Serial.printf("[%s] PullMessages returned an unrecognized response %u time(s) in a row - "
                  "treating the subscription as dead, will resubscribe.\n",
                  cfg.name.c_str(), (unsigned)st.pullAmbiguousStreak);
    { CameraStateLock lock(st); st.subscriptionActive = false; }
    st.pullPointUrl = "";
    st.pullAmbiguousStreak = 0;
  }
  return false;
}

bool cameraRenewSubscription(const CameraConfig& cfg, CameraState& st) {
  if (!st.subscriptionActive || st.pullPointUrl.length() == 0) return false; // same-task read, no lock needed

  String action = "http://docs.oasis-open.org/wsn/bw-2/Renew";
  String body = "<wsnt:Renew><wsnt:TerminationTime>PT5M</wsnt:TerminationTime></wsnt:Renew>";
  String response = cameraSoapCall(cfg, st, st.pullPointUrl, st.pullPointUrl, action, body);

  if (response.indexOf("RenewResponse") >= 0) {
    st.lastRenew = millis();
    Serial.printf("[%s] Subscription renewed.\n", cfg.name.c_str());
    return true;
  }

  Serial.printf("[%s] Renew failed, will resubscribe.\n", cfg.name.c_str());
  { CameraStateLock lock(st); st.subscriptionActive = false; }
  st.pullPointUrl = "";
  return false;
}

bool cameraSetupSequence(const CameraConfig& cfg, CameraState& st) {
  if (!cameraDiscoverServices(cfg, st)) return false;
  if (!cameraFetchProfileAndSnapshotUri(cfg, st)) {
    Serial.printf("[%s] Snapshot URI not resolved - motion will still be detected "
                  "and logged, but photo alerts won't work until this is fixed.\n", cfg.name.c_str());
    // not returning false: detection/logging still work without it
  }
  if (!cameraGetEventServiceCapabilities(cfg, st)) return false;
  String topics, unusedTopics;
  if (!cameraGetEventProperties(cfg, st, &topics, &unusedTopics)) return false;
  // Read by the dashboard, so written under the lock.
  { CameraStateLock lock(st); st.supportedEventTopics = topics; st.unusedEventTopics = unusedTopics; }
  if (!cameraCreatePullPoint(cfg, st)) return false;
  return true;
}

// Resubscribe backoff cap, further limited to half the camera's offline
// threshold. A fault still counts as contact, but only once per retry: with
// both at 5 minutes, backed-off cameras false-alarmed as OFFLINE.
static const unsigned long RETRY_BACKOFF_MAX_MS = 300000UL; // 5 minutes between retries

// Clamped at use. Near 0 would open a new connection ("Connection: close") in
// a tight loop.
static unsigned long safePollIntervalMs(const CameraConfig& cfg) {
  unsigned long ms = cfg.pollIntervalMs;
  if (ms < CAMERA_POLL_INTERVAL_MIN_MS) ms = CAMERA_POLL_INTERVAL_MIN_MS;
  if (ms > CAMERA_POLL_INTERVAL_MAX_MS) ms = CAMERA_POLL_INTERVAL_MAX_MS;
  return ms;
}

// ============================================================
// Per-camera task, pinned to core 1 (away from WiFi and loopTask on core 0).
// Never returns except on a stop request or missing credentials.
// ============================================================
void cameraTaskFn(void* pvParameters) {
  CameraTaskContext* ctx = static_cast<CameraTaskContext*>(pvParameters);
  CameraConfig& cfg = *ctx->cfg; // non-const - see CameraTaskContext::cfg's comment
  CameraState& st = *ctx->st;
  delete ctx; // context struct's job is done once we've unpacked it

  // A live-enabled camera's real config arrives via pendingConfig; no-op at
  // boot.
  applyPendingConfigIfAny(cfg, st);

  Serial.printf("[%s] Task started.\n", cfg.name.c_str());

  // Missing credentials are a config mistake, not a network problem, so exit
  // rather than retry forever.
  if (!resolveCameraCredentials(cfg, st)) {
    Serial.printf("[%s] FATAL: no credentials resolved - task exiting, camera will NOT be monitored "
                  "until this is fixed via the web UI and the board is rebooted.\n", cfg.name.c_str());
    String cameraName = cfg.name;
    sendTelegramMessage([cameraName](TelegramLang lang) { return trMissingCredentials(lang, cameraName, false); });
    vTaskDelete(nullptr);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!cameraSetupSequence(cfg, st)) {
      Serial.printf("[%s] Initial setup FAILED - will keep retrying.\n", cfg.name.c_str());
    }
  }

  // Task-start baselines for the motion watchdog, timelapse and subscription
  // health, so none of them fires just because the task is new.
  st.lastMotionMs = millis();

  st.lastTimelapseMs = millis();

  st.lastSubscribedMs = millis();

  for (;;) {
    // Checked before a pending edit, so a stop wins.
    bool stopNow;
    { CameraStateLock lock(st); stopNow = st.stopRequested; st.stopRequested = false; }
    if (stopNow) {
      cfg.enabled = false; // only this task may write its own cfg - see requestLiveConfigReload's comment
      { CameraStateLock lock(st); st.subscriptionActive = false; }
      Serial.printf("[%s] Stopped via dashboard (disabled/deleted) - task exiting, no reboot needed.\n",
                    cfg.name.c_str());
      logEvent(cfg.name + ": monitoring stopped live (disabled/deleted via dashboard)");
      vTaskDelete(nullptr);
      return;
    }

    // Live edit: applied within one loop pass.
    if (applyPendingConfigIfAny(cfg, st)) {
      Serial.printf("[%s] Configuration changed via dashboard - reconnecting with the new settings.\n",
                    cfg.name.c_str());
      logEvent(cfg.name + ": configuration updated live, reconnecting");
      // Any URL/token may be stale after an edit, so rediscover everything via
      // the normal resubscribe path. snapshotUri is locked because /snap reads
      // it from loop()'s task.
      { CameraStateLock lock(st); st.subscriptionActive = false; st.snapshotUri = ""; }
      st.eventServiceUrl = ""; st.mediaServiceUrl = ""; st.pullPointUrl = ""; st.profileToken = "";
      st.retryDelayMs = 0; st.retryStreak = 0; st.lastRetry = 0; // retry immediately, not after a stale backoff

      if (cfg.user.length() == 0 || cfg.pass.length() == 0) {
        // Unlike at startup, don't exit over a bad edit; stay unsubscribed
        // until fixed.
        Serial.printf("[%s] ERROR: no username/password after this edit - camera will NOT be monitored "
                      "until this is fixed via the web UI.\n", cfg.name.c_str());
        String cameraName = cfg.name;
        sendTelegramMessage([cameraName](TelegramLang lang) { return trMissingCredentials(lang, cameraName, true); });
      }
    }

    if (WiFi.status() != WL_CONNECTED) {
      // loop() reconnects WiFi; afterwards this task resubscribes (the old one
      // has likely expired).
      if (st.subscriptionActive) { // same-task read, no lock needed
        { CameraStateLock lock(st); st.subscriptionActive = false; }
        st.pullPointUrl = "";
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // Refreshed every pass while subscribed, even when polling is skipped
    // below, so subscription health doesn't go stale while snapshots are busy.
    if (st.subscriptionActive) st.lastSubscribedMs = millis();

    // A snapshot GET is in flight to this camera; skip SOAP this pass rather
    // than open a second connection. The health checks below still run.
    bool snapshotBusy;
    { CameraStateLock lock(st); snapshotBusy = st.snapshotInFlight; }

    if (snapshotBusy) {
    } else if (!st.subscriptionActive) {
      unsigned long dueInterval = (st.retryDelayMs > 0) ? st.retryDelayMs : RETRY_INTERVAL_MS;
      if (millis() - st.lastRetry >= dueInterval) {
        // +/-2s jitter so cameras that failed together don't retry in
        // lockstep.
        st.lastRetry = millis() - (unsigned long)random(0, 2001);
        Serial.printf("[%s] Retrying subscription...\n", cfg.name.c_str());
        if (st.eventServiceUrl.length() == 0) {
          cameraSetupSequence(cfg, st); // full rediscovery if we never got services
        } else {
          cameraGetEventServiceCapabilities(cfg, st) && cameraCreatePullPoint(cfg, st);
        }

        // Both paths set subscriptionActive on success.
        if (st.subscriptionActive) {
          Serial.printf("[%s] Subscription recovered.\n", cfg.name.c_str());
          st.retryStreak = 0;
          st.retryDelayMs = 0;
          // Read by the dashboard, so locked.
          {
            CameraStateLock lock(st);
            st.totalReconnects++;
            st.reconnectHistory[st.reconnectHistoryNext] = millis();
            st.reconnectHistoryNext = (st.reconnectHistoryNext + 1) % EVENT_HISTORY_RING_SIZE;
            if (st.reconnectHistoryCount < EVENT_HISTORY_RING_SIZE) st.reconnectHistoryCount++;
          }
        } else {
          // See RETRY_BACKOFF_MAX_MS.
          unsigned long retryBackoffCap =
              detectorSafeBackoffCapMs(RETRY_BACKOFF_MAX_MS, cfg.offlineThresholdMs, RETRY_INTERVAL_MS);
          st.retryDelayMs = nextBackoffDelayMs(st.retryDelayMs, RETRY_INTERVAL_MS, retryBackoffCap);
          st.retryStreak++;
          Serial.printf("[%s] Still not subscribed after %u consecutive attempt(s) - next retry in %lus.\n",
                        cfg.name.c_str(), (unsigned)st.retryStreak, st.retryDelayMs / 1000UL);
        }
      }
    } else {
      if (millis() - st.lastPull >= safePollIntervalMs(cfg)) {
        st.lastPull = millis();
        cameraPullMessages(cfg, st);
      }
      if (millis() - st.lastRenew >= (SUBSCRIPTION_LIFETIME_MS - RENEW_MARGIN_MS)) {
        cameraRenewSubscription(cfg, st);
      }
      if (cfg.timelapseIntervalMin > 0 &&
          millis() - st.lastTimelapseMs >= (unsigned long)cfg.timelapseIntervalMin * 60000UL) {
        st.lastTimelapseMs = millis();
        triggerTimelapseCapture(cfg, st);
      }
      // The first profile/snapshot URI fetch failed and nothing else retries
      // it; without this, photos stay broken while motion keeps working.
      if (st.snapshotUri.length() == 0 &&
          millis() - st.lastSnapshotUriRetryMs >= SNAPSHOT_URI_RETRY_INTERVAL_MS) {
        st.lastSnapshotUriRetryMs = millis();
        if (cameraFetchProfileAndSnapshotUri(cfg, st)) {
          Serial.printf("[%s] Snapshot URI resolved on retry - photo alerts/timelapse now available.\n",
                        cfg.name.c_str());
        }
      }
    }

    checkCameraOnlineStatus(cfg, st);
    checkSubscriptionHealth(cfg, st); // after checkCameraOnlineStatus - reads its just-updated st.isOffline
    checkMotionWatchdog(cfg, st);
    checkPendingMotionDigest(cfg, st);
    // Global check; any camera's loop serves as its clock.
    checkMultiCameraAlertDigest();

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
