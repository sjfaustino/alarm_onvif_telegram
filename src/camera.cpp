#include "camera.h"
#include "onvif_soap.h"
#include "telegram.h"
#include "backoff.h"
#include "camera_parse.h"
#include <WiFi.h>
#include <vector>
#include <cstring>

void cameraStateInit(CameraState& st) {
  if (!st.stateMutex) st.stateMutex = xSemaphoreCreateMutex();
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

// Builds the envelope and posts it, honoring cfg.useWSSecurity (WSSE header
// vs. plain HTTP Basic Auth, for stacks that choke on WSSE). Every SOAP
// call funnels through here, so this is also where st.lastContactMs gets
// updated - a non-empty response (even a SOAP fault) means the camera's
// stack answered, which is what checkCameraOnlineStatus (telegram.cpp)
// uses to tell a genuinely offline camera from one merely failing a call.
static String cameraSoapCall(const CameraConfig& cfg, CameraState& st, const String& url,
                              const String& to, const String& action, const String& body) {
  const char* user; const char* pass;
  { CameraStateLock lock(st); user = st.user; pass = st.pass; }
  String xml = soapEnvelope(action, body, to, user, pass,
                             cfg.includeReplyToAnonymous, cfg.useWSSecurity);
  const char* basicUser = cfg.useWSSecurity ? nullptr : user;
  const char* basicPass = cfg.useWSSecurity ? nullptr : pass;
  String response = soapPost(cfg.name.c_str(), url, action, xml, basicUser, basicPass);
  if (response.length() > 0) st.lastContactMs = millis();
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

bool cameraGetEventProperties(const CameraConfig& cfg, CameraState& st) {
  String action = "http://www.onvif.org/ver10/events/wsdl/EventPortType/GetEventPropertiesRequest";
  String body = "<tev:GetEventProperties/>";
  String response = cameraSoapCall(cfg, st, st.eventServiceUrl, st.eventServiceUrl, action, body);

  if (response.length() == 0 || responseHasFault(response) ||
      response.indexOf("GetEventPropertiesResponse") < 0) {
    Serial.printf("[%s] GetEventProperties FAILED\n", cfg.name.c_str());
    return false;
  }
  return true;
}

bool cameraFetchProfileAndSnapshotUri(const CameraConfig& cfg, CameraState& st) {
  Serial.printf("\n[%s] Resolving snapshot URI\n", cfg.name.c_str());

  if (cfg.snapshotUriOverride.length() > 0) {
    // {USER}/{PASS} let an override embed query-string auth (some Vstarcam
    // firmwares want ?loginuse=...&loginpas=...) without a credential
    // landing in a committed file.
    String resolved = cfg.snapshotUriOverride;
    {
      CameraStateLock lock(st);
      resolved.replace("{USER}", st.user);
      resolved.replace("{PASS}", st.pass);
      st.snapshotUri = resolved;
    }
    String logUri = cfg.snapshotUriOverride; // log the un-substituted form - avoids echoing st.pass to serial
    logUri.replace("{PASS}", "***");
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

// Logs extractEventStateValue's result for topicKeyword (camera_parse.h) -
// the actual search logic is there and unit-tested; this is just the
// Serial-log wrapper camera.cpp needs.
static void printEventState(const CameraConfig& cfg, const String& xml, const String& topicKeyword) {
  String state = extractEventStateValue(xml, topicKeyword);
  if (state.length() > 0) Serial.printf("[%s] State = %s\n", cfg.name.c_str(), state.c_str());
}

static void parseEvents(const CameraConfig& cfg, CameraState& st, const String& xml) {
  CameraEventClassification ev = classifyCameraEvent(xml);
  if (!ev.anyTrue && !VERBOSE_SOAP_LOG) return;
  if (!ev.motionAlarm && !ev.cellMotion && !ev.signalLoss && !ev.tamper) return;

  if (ev.motionAlarm) { Serial.printf("[%s] MOTION ALARM EVENT\n", cfg.name.c_str()); printEventState(cfg, xml, "MotionAlarm"); }
  if (ev.cellMotion)  { Serial.printf("[%s] CELL MOTION EVENT\n", cfg.name.c_str());  printEventState(cfg, xml, "CellMotionDetector"); }
  if (ev.signalLoss)  { Serial.printf("[%s] SIGNAL LOSS EVENT\n", cfg.name.c_str());  printEventState(cfg, xml, "SignalLoss"); }
  if (ev.tamper)      { Serial.printf("[%s] TAMPER EVENT\n", cfg.name.c_str());       printEventState(cfg, xml, "TamperDetector"); }

  // Doesn't distinguish which topic was true if several arrive in the same
  // batch - would need per-NotificationMessage parsing to fix.
  if ((ev.motionAlarm || ev.cellMotion) && ev.anyTrue) {
    triggerMotionAlert(cfg, st);
  }
}

bool cameraPullMessages(const CameraConfig& cfg, CameraState& st) {
  if (!st.subscriptionActive || st.pullPointUrl.length() == 0) return false; // same-task read, no lock needed

  String action = "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest";
  // PT1S long-poll; PT5S (fewer, longer polls) would cut request volume
  // per camera if that's ever worth trading off against latency.
  String body = "<tev:PullMessages><tev:Timeout>PT1S</tev:Timeout>"
                "<tev:MessageLimit>20</tev:MessageLimit></tev:PullMessages>";
  String response = cameraSoapCall(cfg, st, st.pullPointUrl, st.pullPointUrl, action, body);

  if (response.length() == 0) return false;

  if (response.indexOf("PullMessagesResponse") >= 0) {
    parseEvents(cfg, st, response);
    return true;
  }

  if (response.indexOf("ResourceUnknownFault") >= 0 || responseHasFault(response)) {
    Serial.printf("[%s] PullPoint gone, will resubscribe.\n", cfg.name.c_str());
    { CameraStateLock lock(st); st.subscriptionActive = false; }
    st.pullPointUrl = "";
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
    // deliberately not returning false: detection/logging still has value
  }
  if (!cameraGetEventServiceCapabilities(cfg, st)) return false;
  if (!cameraGetEventProperties(cfg, st)) return false;
  if (!cameraCreatePullPoint(cfg, st)) return false;
  return true;
}

// Cap for the per-camera subscription retry backoff below - see
// CameraState::retryDelayMs's comment in camera.h.
static const unsigned long RETRY_BACKOFF_MAX_MS = 300000UL; // 5 minutes between retries

// ============================================================
// Per-camera FreeRTOS task - one per enabled camera, created once in
// setup() and pinned to core 1. Runs forever; never returns. Pinning to
// core 1 gives true parallel execution: one camera's slow SOAP call or
// Telegram TLS upload doesn't steal core 1 time from another's task, and
// neither competes with the WiFi/BT stack or loopTask on core 0.
// ============================================================
void cameraTaskFn(void* pvParameters) {
  CameraTaskContext* ctx = static_cast<CameraTaskContext*>(pvParameters);
  const CameraConfig& cfg = *ctx->cfg;
  CameraState& st = *ctx->st;
  delete ctx; // context struct's job is done once we've unpacked it

  Serial.printf("[%s] Task started.\n", cfg.name.c_str());

  // Resolve credentials once, before anything else. A mismatch here is a
  // config mistake, not a flaky network condition - retrying it forever
  // would just spam the log, so this camera's task exits instead. The
  // Telegram alert doesn't need this camera's credentials to send.
  if (!resolveCameraCredentials(cfg, st)) {
    Serial.printf("[%s] FATAL: no credentials resolved - task exiting, camera will NOT be monitored "
                  "until this is fixed via the web UI and the board is rebooted.\n", cfg.name.c_str());
    sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F " + cfg.name +
                         ": no username/password set for this camera - it is NOT being monitored.");
    vTaskDelete(nullptr);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!cameraSetupSequence(cfg, st)) {
      Serial.printf("[%s] Initial setup FAILED - will keep retrying.\n", cfg.name.c_str());
    }
  }

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      // main.cpp's loop() owns reconnecting; this task just waits and, once
      // back, treats itself as needing a fresh subscription (the old one
      // almost certainly timed out server-side during the outage anyway).
      if (st.subscriptionActive) { // same-task read, no lock needed
        { CameraStateLock lock(st); st.subscriptionActive = false; }
        st.pullPointUrl = "";
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!st.subscriptionActive) {
      unsigned long dueInterval = (st.retryDelayMs > 0) ? st.retryDelayMs : RETRY_INTERVAL_MS;
      if (millis() - st.lastRetry >= dueInterval) {
        // Jitter the retry timer (+/- up to 2s) so cameras that failed
        // together at boot or during a WiFi outage don't stay locked in
        // lockstep, all retrying (and all failing again, if the network's
        // still overwhelmed) on the same tick forever.
        st.lastRetry = millis() - (unsigned long)random(0, 2001);
        Serial.printf("[%s] Retrying subscription...\n", cfg.name.c_str());
        if (st.eventServiceUrl.length() == 0) {
          cameraSetupSequence(cfg, st); // full rediscovery if we never got services
        } else {
          cameraGetEventServiceCapabilities(cfg, st) && cameraCreatePullPoint(cfg, st);
        }

        // Both calls above set st.subscriptionActive=true on success (see
        // cameraCreatePullPoint) as a side effect, so it's the signal here
        // regardless of which branch ran. Same-task read, no lock needed.
        if (st.subscriptionActive) {
          Serial.printf("[%s] Subscription recovered.\n", cfg.name.c_str());
          st.retryStreak = 0;
          st.retryDelayMs = 0;
        } else {
          st.retryDelayMs = nextBackoffDelayMs(st.retryDelayMs, RETRY_INTERVAL_MS, RETRY_BACKOFF_MAX_MS);
          st.retryStreak++;
          Serial.printf("[%s] Still not subscribed after %u consecutive attempt(s) - next retry in %lus.\n",
                        cfg.name.c_str(), (unsigned)st.retryStreak, st.retryDelayMs / 1000UL);
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

    checkCameraOnlineStatus(cfg, st);

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
