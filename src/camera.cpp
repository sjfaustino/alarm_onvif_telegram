#include "camera.h"
#include "onvif_soap.h"
#include "telegram.h"
#include "backoff.h"
#include "camera_parse.h"
#include "event_log_store.h"
#include <WiFi.h>
#include <vector>
#include <cstring>

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

// Applies a staged pendingConfig, if any, to cfg/st - shared by
// cameraTaskFn's startup (for a task spawned straight into an already-
// enabled camera - see webserver_cameras.cpp's live-spawn path, which
// stages the real config here rather than writing g_cameras[idx] directly
// from the webserver task) and its main loop (for an edit applied while
// the task is already running). Returns whether one was applied - the
// loop uses that to know whether to also reset subscription/retry state,
// which the startup path doesn't need (nothing's been set up yet there).
//
// See requestLiveConfigReload's comment (camera.h) for why only this
// owning task may ever perform cfg's assignment - and just as
// importantly, why nothing else may ever write cfg unlocked either: a
// camera spawned live starts this function with cfg still holding
// whatever g_cameras[idx] held before (a stale, previously-disabled
// snapshot) until this runs, rather than main.cpp/webserver_cameras.cpp
// writing the real values into g_cameras[idx] directly and racing any
// other task that reads cameras[i].enabled/.name for that slot without a
// lock (CameraConfig itself has no locking of its own - only the fields
// listed on CameraState::stateMutex do).
static bool applyPendingConfigIfAny(CameraConfig& cfg, CameraState& st) {
  CameraConfig* pending = nullptr;
  { CameraStateLock lock(st); pending = st.pendingConfig; st.pendingConfig = nullptr; }
  if (!pending) return false;

  cfg = *pending; // safe: this task is the sole writer of its own cfg - see requestLiveConfigReload's comment
  delete pending;

  // Unconditional, regardless of whether the new credentials turn out to
  // be valid - st.user/st.pass are raw pointers into cfg.user/cfg.pass's
  // buffers, and the assignment above may have just freed/reallocated the
  // old ones. Leaving them stale even briefly, gated behind a validity
  // check, would be a use-after-free the instant anything reads through
  // them again.
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

// Builds the envelope and posts it, honoring cfg.useWSSecurity (WSSE header
// vs. plain HTTP Basic Auth, for stacks that choke on WSSE). Every SOAP
// call funnels through here, so this is also where st.lastContactMs gets
// updated - a non-empty response (even a SOAP fault) means the camera's
// stack answered, which is what checkCameraOnlineStatus (telegram.cpp)
// uses to tell a genuinely offline camera from one merely failing a call.
// lastContactMs is lock-guarded (CameraState::stateMutex), not same-task-
// only: pushCameraSnapshot (snapshot_history.cpp) also adjusts it, and
// that function is reachable from loop()'s task too (sendOnDemandSnapshot,
// via /snap or handleAllCamerasCommand), not just this camera's own task.
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
  if (!ev.motionAlarm && !ev.cellMotion && !ev.signalLoss && !ev.tamper) {
    // A real notification arrived (not just an empty/heartbeat-ish
    // PullMessagesResponse - see the anyTrue/VERBOSE_SOAP_LOG guard above)
    // but none of the topics this project knows about were in it - e.g. a
    // person/vehicle-detection topic, which has no single standardized
    // name across vendors (see firstTopic's comment). Logged instead of
    // silently dropped, so it's possible to discover what a given camera
    // actually sends and add support for it deliberately.
    if (xml.indexOf("NotificationMessage") >= 0) {
      String topic = firstTopic(xml);
      Serial.printf("[%s] UNRECOGNIZED EVENT - topic: %s (enable VERBOSE_SOAP_LOG to see the full response)\n",
                    cfg.name.c_str(), topic.length() > 0 ? topic.c_str() : "(no Topic element found)");
      logEvent(cfg.name + ": unrecognized ONVIF event" + (topic.length() > 0 ? " (" + topic + ")" : ""));
    }
    return;
  }

  if (ev.motionAlarm) { Serial.printf("[%s] MOTION ALARM EVENT\n", cfg.name.c_str()); printEventState(cfg, xml, "MotionAlarm"); }
  if (ev.cellMotion)  { Serial.printf("[%s] CELL MOTION EVENT\n", cfg.name.c_str());  printEventState(cfg, xml, "CellMotionDetector"); }
  if (ev.signalLoss)  { Serial.printf("[%s] SIGNAL LOSS EVENT\n", cfg.name.c_str());  printEventState(cfg, xml, "SignalLoss"); }
  if (ev.tamper)      { Serial.printf("[%s] TAMPER EVENT\n", cfg.name.c_str());       printEventState(cfg, xml, "TamperDetector"); }

  // Each check below uses topicReportedTrue's per-NotificationMessage
  // scoping, not ev.anyTrue/ev.signalLoss/ev.tamper alone - a body-wide
  // flag or "this topic string is present somewhere" doesn't mean *this*
  // topic's own state is true (see motionEventFired's comment in
  // camera_parse.h for the full reasoning and the field-hit bug this
  // pattern already fixed once for motion).
  if (motionEventFired(xml, ev)) {
    st.lastMotionMs = millis(); // real motion signal, independent of mute/cooldown/quiet hours - see checkMotionWatchdog
    triggerMotionAlert(cfg, st);
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
  // PT1S long-poll; PT5S (fewer, longer polls) would cut request volume
  // per camera if that's ever worth trading off against latency.
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

  // Neither a recognized success nor a recognized fault - a genuinely
  // malformed/unexpected response body (response.length()==0, the
  // transient network-issue case, is already handled above and never
  // reaches here). See pullAmbiguousStreak's own comment (camera.h):
  // without this counter, this camera's task would just keep retrying
  // the same possibly-dead pullPointUrl every PULL_INTERVAL_MS forever,
  // with no path back to a working subscription.
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
    // deliberately not returning false: detection/logging still has value
  }
  if (!cameraGetEventServiceCapabilities(cfg, st)) return false;
  if (!cameraGetEventProperties(cfg, st)) return false;
  if (!cameraCreatePullPoint(cfg, st)) return false;
  return true;
}

// Cap for the per-camera subscription retry backoff below - see
// CameraState::retryDelayMs's comment in camera.h. Also never allowed to
// exceed half of this camera's own offlineThresholdMs (see the retry loop
// below) - a SOAP fault still counts as "contact" (cameraSoapCall's
// comment), so a camera stuck failing subscription retries keeps
// refreshing lastContactMs, but only as often as it's actually retried.
// Hit in the field: with this fixed at 5 minutes and offlineThresholdMs
// defaulting to the same 5 minutes, a camera whose retries had backed off
// to the ceiling could go quiet for just long enough between retries -
// each of which still would have answered - to trip a false OFFLINE
// alert on its own, with nothing actually wrong.
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
  CameraConfig& cfg = *ctx->cfg; // non-const - see CameraTaskContext::cfg's comment
  CameraState& st = *ctx->st;
  delete ctx; // context struct's job is done once we've unpacked it

  // A camera spawned live (webserver_cameras.cpp enabling a previously-
  // disabled one) stages its real configuration via pendingConfig instead
  // of it being written into g_cameras[idx] directly by the webserver
  // task - see applyPendingConfigIfAny's comment for why. No-op (cfg is
  // used as-is) for the normal boot-time spawn path, which never stages one.
  applyPendingConfigIfAny(cfg, st);

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

  // Baseline for checkMotionWatchdog - task-start time, not 0, so a camera
  // that never fires within cfg.motionWatchdogHours after boot still
  // trips (see CameraState::lastMotionMs's own comment).
  st.lastMotionMs = millis();

  // Same reasoning, same fix, for the timelapse interval check below - a
  // 0 baseline would make millis()-0 already exceed a short configured
  // interval for any task whose subscription took longer than that
  // interval to come up (retries, discovery, or a camera enabled live via
  // the dashboard hours after boot), firing an unwanted capture the
  // instant it subscribes instead of waiting a full interval first.
  st.lastTimelapseMs = millis();

  for (;;) {
    // Live-reload from a dashboard edit (webserver_cameras.cpp's save
    // handler, via requestLiveConfigReload) - checked first, every pass,
    // so it takes effect within one ~10ms loop tick of being staged.
    if (applyPendingConfigIfAny(cfg, st)) {
      Serial.printf("[%s] Configuration changed via dashboard - reconnecting with the new settings.\n",
                    cfg.name.c_str());
      logEvent(cfg.name + ": configuration updated live, reconnecting");
      // Every discovered URL/token is potentially stale once the device
      // URL, credentials, or WS-Security mode change - full rediscovery
      // (via the "not subscribed" retry path below, same one a fresh boot
      // or a lost subscription already takes) is the only safe path, not
      // a partial patch-up.
      // snapshotUri specifically needs the lock (unlike eventServiceUrl/
      // mediaServiceUrl/pullPointUrl/profileToken below, which - like
      // every other field this task touches unlocked elsewhere in this
      // file - are read only by this same task): a Telegram /snap
      // command on loop()'s task reads it cross-task via
      // fetchOneSnapshot/sendOnDemandSnapshot (telegram.cpp), under this
      // same lock, and could otherwise race a plain String assignment
      // here mid-read.
      { CameraStateLock lock(st); st.subscriptionActive = false; st.snapshotUri = ""; }
      st.eventServiceUrl = ""; st.mediaServiceUrl = ""; st.pullPointUrl = ""; st.profileToken = "";
      st.retryDelayMs = 0; st.retryStreak = 0; st.lastRetry = 0; // retry immediately, not after a stale backoff

      if (cfg.user.length() == 0 || cfg.pass.length() == 0) {
        // Unlike the same check at task startup above, this does NOT exit
        // the task - a task that's already been happily monitoring a
        // camera for weeks shouldn't die outright over one bad edit. It
        // just stays not-subscribed (every SOAP call below will cleanly
        // fail and retry, same as an unreachable camera) until fixed via
        // another edit.
        Serial.printf("[%s] ERROR: no username/password after this edit - camera will NOT be monitored "
                      "until this is fixed via the web UI.\n", cfg.name.c_str());
        sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F " + cfg.name +
                             ": no username/password set for this camera after the last edit - it is "
                             "NOT being monitored. Fix it via the dashboard.");
      }
    }

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
          // Lock-guarded write, unlike retryStreak/retryDelayMs above -
          // totalReconnects is read cross-task by the dashboard (see its
          // own comment, camera.h).
          { CameraStateLock lock(st); st.totalReconnects++; }
        } else {
          // Never let the retry cadence alone go slower than half this
          // camera's offline threshold - see RETRY_BACKOFF_MAX_MS's and
          // detectorSafeBackoffCapMs's (backoff.h) comments for why.
          unsigned long retryBackoffCap =
              detectorSafeBackoffCapMs(RETRY_BACKOFF_MAX_MS, cfg.offlineThresholdMs, RETRY_INTERVAL_MS);
          st.retryDelayMs = nextBackoffDelayMs(st.retryDelayMs, RETRY_INTERVAL_MS, retryBackoffCap);
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
      // Only once actually subscribed - same reasoning as lastPull/
      // lastRenew above, no point capturing before a snapshot URI even
      // exists (triggerTimelapseCapture no-ops on that anyway, but no
      // reason to even try before subscription succeeds once).
      if (cfg.timelapseIntervalMin > 0 &&
          millis() - st.lastTimelapseMs >= (unsigned long)cfg.timelapseIntervalMin * 60000UL) {
        st.lastTimelapseMs = millis();
        triggerTimelapseCapture(cfg, st);
      }
      // Subscribed and polling fine, but the very first
      // cameraFetchProfileAndSnapshotUri call (cameraSetupSequence) failed
      // and nothing else ever retries it once eventServiceUrl is set (see
      // CameraState::lastSnapshotUriRetryMs's own comment) - without this,
      // a single transient GetProfiles/GetSnapshotUri failure permanently
      // breaks photo alerts/timelapse for this camera while motion
      // detection keeps working normally, masking the problem.
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
    checkMotionWatchdog(cfg, st);

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
