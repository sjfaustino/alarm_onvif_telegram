#include "telegram.h"
#include "telegram_internal.h"
#include "config.h"
#include "subscription_health.h"
#include "telegram_users.h"
#include "telegram_i18n.h"
#include "format_utils.h"
#include "event_log_store.h"
#include "snapshot_history.h"
#include "sd_store.h"
#include "quiet_hours.h"
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>
#include <algorithm>

// Clamped at use (imported configs bypass the form). 0 isn't "off": it would
// alert on every poll with no digest throttling - the multi-camera burst that
// once broke TLS.
static unsigned long safeAlertCooldownMs(const CameraConfig& cfg) {
  unsigned long ms = cfg.alertCooldownMs;
  if (ms == 0) ms = CameraConfig().alertCooldownMs;
  if (ms > CAMERA_ALERT_COOLDOWN_MAX_MS) ms = CAMERA_ALERT_COOLDOWN_MAX_MS;
  return ms;
}

// 0 would mark every camera OFFLINE permanently.
static unsigned long safeOfflineThresholdMs(const CameraConfig& cfg) {
  unsigned long ms = cfg.offlineThresholdMs;
  if (ms == 0) ms = CameraConfig().offlineThresholdMs;
  if (ms > CAMERA_OFFLINE_THRESHOLD_MAX_MS) ms = CAMERA_OFFLINE_THRESHOLD_MAX_MS;
  return ms;
}

// Shots go out back-to-back, so an unclamped count floods Telegram.
static unsigned int safeSnapshotBurstCount(const CameraConfig& cfg) {
  unsigned int shots = (cfg.snapshotBurstCount > 0) ? cfg.snapshotBurstCount : 1;
  if (shots > CAMERA_SNAPSHOT_BURST_MAX) shots = CAMERA_SNAPSHOT_BURST_MAX;
  return shots;
}

// Just what the alert paths need (chat and language), not the full
// TelegramUser with its camera-name vector - avoids heap churn on every alert.
struct AlertRecipient {
  String chatId;
  TelegramLang language;
};

// Cross-camera digest window, shared by all camera tasks.
static SemaphoreHandle_t g_multiCameraDigestMutex = xSemaphoreCreateMutex();
// 0 = no window open.
static unsigned long g_multiCameraDigestWindowStartMs = 0;
static std::vector<String> g_multiCameraDigestCameras; // distinct camera names seen so far this window

// Adds a camera to the current window (opening one if needed), once per
// camera. Called after a real send is decided.
static void noteMultiCameraAlert(const String& cameraName) {
  xSemaphoreTake(g_multiCameraDigestMutex, portMAX_DELAY);
  if (g_multiCameraDigestWindowStartMs == 0) g_multiCameraDigestWindowStartMs = millis();
  bool alreadyIn = false;
  for (auto& n : g_multiCameraDigestCameras) {
    if (n.equalsIgnoreCase(cameraName)) { alreadyIn = true; break; }
  }
  if (!alreadyIn) g_multiCameraDigestCameras.push_back(cameraName);
  xSemaphoreGive(g_multiCameraDigestMutex);
}

// English label for the Activity log (which is always English).
static const char* motionKindLogLabel(MotionDetectionKind kind) {
  switch (kind) {
    case MotionDetectionKind::Person:  return "person";
    case MotionDetectionKind::Vehicle: return "vehicle";
    case MotionDetectionKind::Generic: return "motion";
  }
  return "motion"; // unreachable if every enumerator above is handled
}

static SnapshotSource snapshotSourceForMotion(bool isPetEvent, MotionDetectionKind kind) {
  if (isPetEvent) return SnapshotSource::Pet;
  switch (kind) {
    case MotionDetectionKind::Person:  return SnapshotSource::Person;
    case MotionDetectionKind::Vehicle: return SnapshotSource::Vehicle;
    case MotionDetectionKind::Generic: return SnapshotSource::Motion;
  }
  return SnapshotSource::Motion; // unreachable if every enumerator above is handled
}

// Caller holds st's lock.
static void incrementDigestCounter(CameraState& st, bool isPetEvent, MotionDetectionKind kind) {
  if (isPetEvent) { st.digestPetCount++; return; }
  switch (kind) {
    case MotionDetectionKind::Person:  st.digestPersonCount++;  return;
    case MotionDetectionKind::Vehicle: st.digestVehicleCount++; return;
    case MotionDetectionKind::Generic: st.digestMotionCount++;  return;
  }
}

void triggerMotionAlert(const CameraConfig& cfg, CameraState& st, bool isPetEvent, MotionDetectionKind kind) {
  // Written by loop() (/on /off), so read under the lock.
  bool alertsEnabled;
  { CameraStateLock lock(st); alertsEnabled = st.alertsEnabled; }
  if (!alertsEnabled) return; // muted via Telegram - see pollTelegramCommands

  // lastAlert/hasAlerted are written only on this task; only other tasks need
  // the lock to read them.
  uint32_t nowMs = millis();
  if (st.hasAlerted && nowMs - st.lastAlert < safeAlertCooldownMs(cfg)) {
    // Count motion during the cooldown only if a real photo started it (not
    // quiet hours). Pet events aren't counted: the digest is about motion.
    if (st.digestArmed && !isPetEvent) {
      st.suppressedMotionCount++;
      st.lastSuppressedMotionMs = nowMs; // when THIS event happened, not when the digest will flush
    }
    return; // cooling down
  }

  // Quiet hours suppress only the send; detection, logging and the snapshot
  // still happen. Tamper/signal loss ignore quiet hours.
  bool quiet = cfg.quietHoursEnabled && localClockSynced() &&
               isWithinQuietHours(currentLocalMinuteOfDay(), cfg.quietStartMinute, cfg.quietEndMinute);
  if (quiet) {
    // Don't spend the cooldown when there's nothing to capture.
    if (st.snapshotUri.length() == 0) return;
    // Must disarm the digest here: lastAlert moves forward, and a stale
    // lastSuppressedMotionMs would underflow the digest's elapsed time.
    st.digestArmed = false;
    st.suppressedMotionCount = 0;
    { CameraStateLock lock(st); st.lastAlert = nowMs; st.hasAlerted = true;
      incrementDigestCounter(st, isPetEvent, kind); }
    logEvent(cfg.name + ": " + (isPetEvent ? "pet" : motionKindLogLabel(kind)) +
             " detected (quiet hours - no Telegram alert)");
    size_t jpgLen = 0;
    uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
    if (jpg) {
      pushCameraSnapshot(cfg, st, jpg, jpgLen, snapshotSourceForMotion(isPetEvent, kind)); // takes ownership
    }
    return;
  }

  std::vector<AlertRecipient> recipients;
  for (auto& u : loadTelegramUsers()) {
    if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back({u.chatId, u.language});
  }
  if (recipients.empty()) {
    Serial.printf("[%s] No Telegram user is subscribed to this camera - skipping send.\n", cfg.name.c_str());
    return;
  }

  // Text-only pet alert: the snapshot is still stored, just not sent.
  if (isPetEvent && cfg.petAlertsTextOnly) {
    { CameraStateLock lock(st); st.lastAlert = nowMs; st.hasAlerted = true;
      incrementDigestCounter(st, isPetEvent, kind); }
    st.digestArmed = true;
    st.suppressedMotionCount = 0;
    String timestamp = nowTimestampString();
    for (auto& r : recipients) sendTelegramMessageTo(r.chatId, trPetAlertText(r.language, cfg.name, timestamp));
    logEvent(cfg.name + ": pet alert (text) to " + String(recipients.size()) + " recipient(s)");
    noteMultiCameraAlert(cfg.name);
    if (st.snapshotUri.length() > 0) {
      size_t jpgLen = 0;
      uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
      if (jpg) pushCameraSnapshot(cfg, st, jpg, jpgLen, SnapshotSource::Pet); // takes ownership
    }
    return;
  }

  if (st.snapshotUri.length() == 0) {
    Serial.printf("[%s] No snapshot URI available - skipping Telegram send.\n", cfg.name.c_str());
    return;
  }

  // Spend the cooldown only once a send is attempted, so an unresolved URI or
  // no subscribers doesn't eat it. A failed send still spends it, to avoid
  // retry storms.
  { CameraStateLock lock(st); st.lastAlert = nowMs; st.hasAlerted = true;
    incrementDigestCounter(st, isPetEvent, kind); }
  // Fresh digest cycle, so nothing is counted twice.
  st.digestArmed = true;
  st.suppressedMotionCount = 0;

  unsigned int shots = safeSnapshotBurstCount(cfg);
  logEvent(cfg.name + ": " + (isPetEvent ? "pet" : motionKindLogLabel(kind)) + " alert, " + String(shots) +
           " shot(s) to " + String(recipients.size()) + " recipient(s)");
  noteMultiCameraAlert(cfg.name);

  // One fetch per shot, fanned out to all recipients (fetching per recipient
  // would overload 1-2-connection cameras). Fetch and upload time already
  // space the shots.
  for (unsigned int i = 0; i < shots; i++) {
    size_t jpgLen = 0;
    uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
    if (i == 0) {
      // Motion-to-photo latency, to guide pollIntervalMs tuning. Diagnostic
      // only.
      unsigned long latencyMs = millis() - st.lastMotionMs;
      logEvent(cfg.name + ": " + String(latencyMs) +
               "ms from motion to first photo" + (jpg ? "" : " (fetch failed)"));
      if (jpg) {
        // Recorded only for successful fetches.
        CameraStateLock lock(st);
        st.motionLatencyHistory[st.motionLatencyHistoryNext] = latencyMs;
        st.motionLatencyHistoryNext = (st.motionLatencyHistoryNext + 1) % EVENT_HISTORY_RING_SIZE;
        if (st.motionLatencyHistoryCount < EVENT_HISTORY_RING_SIZE) st.motionLatencyHistoryCount++;
      }
    }
    if (!jpg) continue; // one bad shot in a burst shouldn't abort the rest

    String timestamp = nowTimestampString();
    String burstSuffix = shots > 1 ? " (" + String(i + 1) + "/" + String(shots) + ")" : "";

    for (auto& r : recipients) {
      String caption = trMotionCaption(r.language, cfg.name, timestamp, isPetEvent, kind) + burstSuffix;
      if (!sendTelegramPhotoWithRetry(jpg, jpgLen, caption, r.chatId)) {
        Serial.printf("[%s] Telegram send to chat %s failed.\n", cfg.name.c_str(), r.chatId.c_str());
      }
    }
    pushCameraSnapshot(cfg, st, jpg, jpgLen, snapshotSourceForMotion(isPetEvent, kind)); // takes ownership
  }
}

// Dashboard "Send test alert": one fresh snapshot to this camera's
// subscribers, ignoring mute, quiet hours and cooldown, and leaving the real
// alert state untouched. Still stored in history. outDetail gets the failure
// reason.
bool sendTestAlert(const CameraConfig& cfg, CameraState& st, String& outDetail, MotionDetectionKind kind,
                    bool isPetEvent) {
  // Runs on the test-alert task, so read under the lock.
  bool hasSnapshotUri;
  { CameraStateLock lock(st); hasSnapshotUri = st.snapshotUri.length() > 0; }
  if (!hasSnapshotUri) {
    outDetail = "no snapshot URI resolved yet for this camera";
    return false;
  }

  std::vector<AlertRecipient> recipients;
  for (auto& u : loadTelegramUsers()) {
    if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back({u.chatId, u.language});
  }
  if (recipients.empty()) {
    outDetail = "no Telegram user is currently subscribed to this camera";
    return false;
  }

  size_t jpgLen = 0;
  uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
  if (!jpg) {
    outDetail = "snapshot fetch from the camera failed - see the Serial log";
    return false;
  }

  String timestamp = nowTimestampString();
  bool anyOk = false;
  for (auto& r : recipients) {
    if (sendTelegramPhotoWithRetry(jpg, jpgLen, trTestAlertCaption(r.language, cfg.name, timestamp, kind, isPetEvent),
                                    r.chatId)) {
      anyOk = true;
    }
  }
  pushCameraSnapshot(cfg, st, jpg, jpgLen, SnapshotSource::Test); // takes ownership

  if (!anyOk) {
    outDetail = "Telegram delivery failed for every recipient - see the Serial log";
    return false;
  }
  logEvent(cfg.name + ": test alert sent to " + String(recipients.size()) + " recipient(s)");
  return true;
}

void triggerTimelapseCapture(const CameraConfig& cfg, CameraState& st) {
  if (st.snapshotUri.length() == 0) return;

  size_t jpgLen = 0;
  uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
  if (!jpg) return; // logged by fetchOneSnapshot itself

  // Optional Telegram delivery. Doesn't use beginCameraAlert: timelapse has
  // its own schedule and must not touch motion cooldown state. Respects mute
  // and quiet hours (send only; the capture is always stored).
  bool quiet = cfg.quietHoursEnabled && localClockSynced() &&
               isWithinQuietHours(currentLocalMinuteOfDay(), cfg.quietStartMinute, cfg.quietEndMinute);

  if (cfg.timelapseSendToTelegram && !quiet) {
    bool alertsEnabled;
    { CameraStateLock lock(st); alertsEnabled = st.alertsEnabled; }
    if (alertsEnabled) {
      std::vector<AlertRecipient> recipients;
      for (auto& u : loadTelegramUsers()) {
        if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back({u.chatId, u.language});
      }
      // Routine wording, so it's never mistaken for an alert.
      String timestamp = nowTimestampString();
      for (auto& r : recipients) {
        String caption = trTimelapseCaption(r.language, cfg.name, timestamp);
        if (!sendTelegramPhotoWithRetry(jpg, jpgLen, caption, r.chatId)) {
          Serial.printf("[%s] Scheduled snapshot send to chat %s failed.\n", cfg.name.c_str(), r.chatId.c_str());
        }
      }
    }
  }

  pushCameraSnapshot(cfg, st, jpg, jpgLen, SnapshotSource::Timelapse); // takes ownership
}

// Shared by tamper and signal-loss: returns subscribed recipients and spends
// the cooldown, or returns empty (spending nothing) if muted, cooling down or
// unsubscribed. Motion has an extra snapshot-URI gate, so it doesn't use this.
static std::vector<AlertRecipient> beginCameraAlert(const CameraConfig& cfg, CameraState& st, uint32_t nowMs) {
  bool alertsEnabled;
  { CameraStateLock lock(st); alertsEnabled = st.alertsEnabled; }
  if (!alertsEnabled) return {};

  if (st.hasAlerted && nowMs - st.lastAlert < safeAlertCooldownMs(cfg)) return {};

  std::vector<AlertRecipient> recipients;
  for (auto& u : loadTelegramUsers()) {
    if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back({u.chatId, u.language});
  }
  if (recipients.empty()) return {};

  { CameraStateLock lock(st); st.lastAlert = nowMs; st.hasAlerted = true; }
  return recipients;
}

void triggerTamperAlert(const CameraConfig& cfg, CameraState& st) {
  std::vector<AlertRecipient> recipients = beginCameraAlert(cfg, st, millis());
  if (recipients.empty()) return;

  logEvent(cfg.name + ": TAMPER detected");
  String timestamp = nowTimestampString();

  bool hasSnapshotUri;
  { CameraStateLock lock(st); hasSnapshotUri = st.snapshotUri.length() > 0; }
  size_t jpgLen = 0;
  uint8_t* jpg = hasSnapshotUri ? fetchOneSnapshot(cfg, st, jpgLen) : nullptr;

  if (jpg) {
    for (auto& r : recipients) {
      String caption = trTamperCaption(r.language, cfg.name, timestamp);
      if (!sendTelegramPhotoWithRetry(jpg, jpgLen, caption, r.chatId)) {
        Serial.printf("[%s] Tamper alert photo send to chat %s failed.\n", cfg.name.c_str(), r.chatId.c_str());
      }
    }
    pushCameraSnapshot(cfg, st, jpg, jpgLen, SnapshotSource::Tamper); // takes ownership
  } else {
    // No photo available; don't stay silent about tamper.
    for (auto& r : recipients) sendTelegramMessageTo(r.chatId, trTamperCaption(r.language, cfg.name, timestamp));
  }
}

void triggerSignalLossAlert(const CameraConfig& cfg, CameraState& st) {
  std::vector<AlertRecipient> recipients = beginCameraAlert(cfg, st, millis());
  if (recipients.empty()) return;

  logEvent(cfg.name + ": video SIGNAL LOSS");
  String timestamp = nowTimestampString();
  for (auto& r : recipients) sendTelegramMessageTo(r.chatId, trSignalLossMessage(r.language, cfg.name, timestamp));
}

void checkCameraOnlineStatus(const CameraConfig& cfg, CameraState& st) {
  // pushCameraSnapshot adjusts lastContactMs from other tasks, hence the lock.
  unsigned long lastContactMs;
  { CameraStateLock lock(st); lastContactMs = st.lastContactMs; }
  unsigned long offlineThresholdMs = safeOfflineThresholdMs(cfg);
  bool offlineNow = (millis() - lastContactMs) >= offlineThresholdMs;
  // Written only on this task; other tasks read it, so the write is locked.
  if (offlineNow == st.isOffline) return; // no state change - most calls hit this

  {
    CameraStateLock lock(st);
    st.isOffline = offlineNow;
    if (offlineNow) {
      st.offlineHistory[st.offlineHistoryNext] = millis();
      st.offlineHistoryNext = (st.offlineHistoryNext + 1) % EVENT_HISTORY_RING_SIZE;
      if (st.offlineHistoryCount < EVENT_HISTORY_RING_SIZE) st.offlineHistoryCount++;
    }
  }
  if (offlineNow) {
    // Report the threshold actually enforced (possibly clamped).
    Serial.printf("[%s] OFFLINE - no response for over %lus.\n", cfg.name.c_str(), offlineThresholdMs / 1000UL);
    logEvent(cfg.name + ": OFFLINE (no response for over " + String(offlineThresholdMs / 60000UL) + "m)");
    unsigned long minutes = offlineThresholdMs / 60000UL;
    sendTelegramMessage([cfg, minutes](TelegramLang lang) { return trCameraOffline(lang, cfg.name, minutes); });
  } else {
    Serial.printf("[%s] Back ONLINE.\n", cfg.name.c_str());
    logEvent(cfg.name + ": back ONLINE");
    sendTelegramMessage([cfg](TelegramLang lang) { return trCameraBackOnline(lang, cfg.name); });
  }
}

// Same-task state; no lock needed.
void checkSubscriptionHealth(const CameraConfig& cfg, CameraState& st) {
  // Reuses the offline threshold. Decision logic is in
  // lib/subscription_health.
  unsigned long threshold = safeOfflineThresholdMs(cfg);
  SubscriptionHealthResult result =
      evaluateSubscriptionHealth(st.isOffline, millis() - st.lastSubscribedMs, threshold,
                                  st.subscriptionLostAlerted);
  st.subscriptionLostAlerted = result.alerted;
  if (!result.shouldAlert) return;

  Serial.printf("[%s] Responding, but hasn't held a working subscription in over %lus - no events can "
                "be received.\n", cfg.name.c_str(), threshold / 1000UL);
  logEvent(cfg.name + ": responding but not subscribed for over " + String(threshold / 60000UL) +
           "m - not receiving events");
  unsigned long minutes = threshold / 60000UL;
  sendTelegramMessage([cfg, minutes](TelegramLang lang) { return trSubscriptionLost(lang, cfg.name, minutes); });
}

// Same-task state; no lock needed.
void checkMotionWatchdog(const CameraConfig& cfg, CameraState& st) {
  if (cfg.motionWatchdogHours == 0) return; // disabled

  // Clamped at use: hours * 3600000 overflows 32 bits above ~1193h, turning a
  // huge value into a tiny threshold.
  uint32_t safeHours = cfg.motionWatchdogHours;
  if (safeHours > 1000) safeHours = 1000;
  unsigned long thresholdMs = (unsigned long)safeHours * 3600000UL;
  if (millis() - st.lastMotionMs < thresholdMs) {
    st.motionWatchdogTripped = false; // motion resumed since the last trip - re-arm
    return;
  }
  if (st.motionWatchdogTripped) return; // already alerted for this stretch of silence

  st.motionWatchdogTripped = true;
  Serial.printf("[%s] No motion detected in over %u hour(s).\n", cfg.name.c_str(),
                (unsigned)cfg.motionWatchdogHours);
  logEvent(cfg.name + ": no motion detected in over " + String((unsigned)cfg.motionWatchdogHours) + "h");
  unsigned hours = (unsigned)cfg.motionWatchdogHours;
  sendTelegramMessage([cfg, hours](TelegramLang lang) { return trMotionWatchdogTripped(lang, cfg.name, hours); });
}

// After the cooldown, reports how much motion continued, or stays silent if
// none. Same-task state.
void checkPendingMotionDigest(const CameraConfig& cfg, CameraState& st) {
  if (!st.digestArmed) return; // no real send is currently being tracked
  if (millis() - st.lastAlert < safeAlertCooldownMs(cfg)) return; // cooldown still running - not due yet

  st.digestArmed = false; // one flush per cooldown cycle, whatever the count
  uint32_t count = st.suppressedMotionCount;
  unsigned long lastSuppressedMotionMs = st.lastSuppressedMotionMs;
  st.suppressedMotionCount = 0;
  if (count == 0) return; // genuinely a one-off - nothing to report
  // The bookkeeping above always runs; only the send is optional.
  if (!cfg.motionDigestEnabled) return;

  // Written by loop() (/on /off), so read under the lock.
  bool alertsEnabled;
  { CameraStateLock lock(st); alertsEnabled = st.alertsEnabled; }
  if (!alertsEnabled) return; // muted since the snapshot went out - stay quiet

  std::vector<AlertRecipient> recipients;
  for (auto& u : loadTelegramUsers()) {
    if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back({u.chatId, u.language});
  }
  if (recipients.empty()) return;

  // Snapshot to the last suppressed event, not to now: motion that stopped
  // after 10s of a 30s cooldown reports 10s.
  unsigned long elapsedSec = (lastSuppressedMotionMs - st.lastAlert) / 1000UL;
  for (auto& r : recipients) {
    sendTelegramMessageTo(r.chatId, trMotionDigest(r.language, cfg.name, count, elapsedSec));
  }
  logEvent(cfg.name + ": motion digest - " + String(count) + " event(s) in " + String(elapsedSec) + "s");
}

// Swap the window out under the lock; send outside it.
void checkMultiCameraAlertDigest() {
  unsigned long windowStartMs;
  std::vector<String> cameras;
  {
    xSemaphoreTake(g_multiCameraDigestMutex, portMAX_DELAY);
    windowStartMs = g_multiCameraDigestWindowStartMs;
    if (windowStartMs == 0 || millis() - windowStartMs < MULTI_CAMERA_DIGEST_WINDOW_MS) {
      xSemaphoreGive(g_multiCameraDigestMutex);
      return; // no window open, or still within it - not due yet
    }
    cameras = g_multiCameraDigestCameras;
    g_multiCameraDigestWindowStartMs = 0;
    g_multiCameraDigestCameras.clear();
    xSemaphoreGive(g_multiCameraDigestMutex);
  }
  // One camera is already covered by its own digest.
  if (cameras.size() < 2) return;

  String cameraList;
  for (size_t i = 0; i < cameras.size(); i++) cameraList += (i > 0 ? ", " : "") + cameras[i];
  sendTelegramMessage([&](TelegramLang lang) {
    return trMultiCameraDigest(lang, (uint32_t)cameras.size(), cameraList);
  });
  logEvent(String((unsigned)cameras.size()) + " cameras detected motion together: " + cameraList);
}

// Only cameras with activity get a line.
struct DailyDigestTally {
  String name;
  uint32_t person, vehicle, pet, motion;
};

void checkDailyActivityDigest(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  std::vector<DailyDigestTally> tallies;
  for (size_t i = 0; i < numCameras; i++) {
    uint32_t person, vehicle, pet, motion;
    {
      // Read and reset together, so every camera's window restarts at once.
      CameraStateLock lock(states[i]);
      person = states[i].digestPersonCount;
      vehicle = states[i].digestVehicleCount;
      pet = states[i].digestPetCount;
      motion = states[i].digestMotionCount;
      states[i].digestPersonCount = 0;
      states[i].digestVehicleCount = 0;
      states[i].digestPetCount = 0;
      states[i].digestMotionCount = 0;
    }
    if (person == 0 && vehicle == 0 && pet == 0 && motion == 0) continue;
    tallies.push_back({cameras[i].name, person, vehicle, pet, motion});
  }

  if (tallies.empty()) return; // every camera was quiet - nothing worth a notification over

  sendTelegramMessage([tallies](TelegramLang lang) {
    String msg = trDailyDigestHeader(lang);
    for (auto& t : tallies) {
      msg += "\n" + trDailyDigestCameraLine(lang, t.name, t.person, t.vehicle, t.pet, t.motion);
    }
    return msg;
  });
  logEvent("Daily activity digest sent (" + String((unsigned)tallies.size()) + " of " +
           String((unsigned)numCameras) + " camera(s) had activity)");
}

