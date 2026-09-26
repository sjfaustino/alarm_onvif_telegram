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

// Clamped here, at the point of use, not just at the dashboard save
// (webserver_cameras.cpp's parseCameraForm clamps user input to
// [1,CAMERA_ALERT_COOLDOWN_MAX_MS]) - same "hand-edited/imported NVS blob
// bypasses the form entirely" reasoning as motionWatchdogHours above, the
// SD storage check interval, and ntpSyncIntervalMs (main.cpp's setupTime).
// A 0 here isn't a "disabled" sentinel - every motion poll would re-alert
// with no throttling at all, and skip triggerMotionAlert's own digest-
// suppression entirely (it only engages while a cooldown is actually
// running) - exactly the unthrottled multi-camera Telegram burst class
// this project has already been burned by once (see git history around
// "Serialize Telegram TLS sends to fix multi-camera burst SSL failures").
// Import (webserver_security.cpp) writes this field with no clamp of its
// own at all.
static unsigned long safeAlertCooldownMs(const CameraConfig& cfg) {
  unsigned long ms = cfg.alertCooldownMs;
  if (ms == 0) ms = CameraConfig().alertCooldownMs;
  if (ms > CAMERA_ALERT_COOLDOWN_MAX_MS) ms = CAMERA_ALERT_COOLDOWN_MAX_MS;
  return ms;
}

// Same idea as safeAlertCooldownMs. A 0 here would make
// checkCameraOnlineStatus's offlineNow computation (millis() -
// lastContactMs >= 0) always true, permanently mis-showing a healthy
// camera as OFFLINE instead of the throttling actually intended.
static unsigned long safeOfflineThresholdMs(const CameraConfig& cfg) {
  unsigned long ms = cfg.offlineThresholdMs;
  if (ms == 0) ms = CameraConfig().offlineThresholdMs;
  if (ms > CAMERA_OFFLINE_THRESHOLD_MAX_MS) ms = CAMERA_OFFLINE_THRESHOLD_MAX_MS;
  return ms;
}

// Same idea as safeAlertCooldownMs. An unclamped burst count sent with no
// delay between shots (see triggerMotionAlert's own comment below) is
// exactly the "flooding past Telegram's rate limit" the dashboard form's
// own clamp exists to prevent.
static unsigned int safeSnapshotBurstCount(const CameraConfig& cfg) {
  unsigned int shots = (cfg.snapshotBurstCount > 0) ? cfg.snapshotBurstCount : 1;
  if (shots > CAMERA_SNAPSHOT_BURST_MAX) shots = CAMERA_SNAPSHOT_BURST_MAX;
  return shots;
}

// A camera-alert recipient carries only what triggerMotionAlert/
// triggerTimelapseCapture/triggerTamperAlert/triggerSignalLossAlert/
// checkPendingMotionDigest actually need after telegramUserWantsCamera has
// already decided this user is included: where to send it, and in what
// language. Deliberately NOT the full TelegramUser - that struct also
// carries a std::vector<String> cameraNames (its own heap allocation for
// any user with an explicit camera list) plus five more fields none of
// these alert paths touch, and copying all of that into a fresh vector on
// every single motion/tamper/timelapse/digest event, for every matching
// recipient, is exactly the kind of unnecessary heap churn this project
// is otherwise careful about (see checkHeapHealth/HEAP_LOW_WARN_BYTES,
// sendTelegramPhotoBuffered's own "buffered once, not per-recipient"
// reasoning). handleTelegramCommand/handleTelegramCallbackQuery and
// friends still use the full TelegramUser - they're single-recipient, and
// already need every field (permissions, name, ...) for that one reply.
struct AlertRecipient {
  String chatId;
  TelegramLang language;
};

// Multi-camera alert digest - correlates DIFFERENT cameras alerting close
// together (see checkMultiCameraAlertDigest's own comment, telegram.h) into
// one extra summary message. Cross-task shared state: each camera runs on
// its own FreeRTOS task (cameraTaskFn), so this needs its own mutex, same
// pattern as g_telegramNetMutex/g_commandRateMutex elsewhere in this file.
static SemaphoreHandle_t g_multiCameraDigestMutex = xSemaphoreCreateMutex();
// 0 = no window currently open - opened by the first camera to alert,
// closed (and cleared) by checkMultiCameraAlertDigest once
// MULTI_CAMERA_DIGEST_WINDOW_MS has passed since then.
static unsigned long g_multiCameraDigestWindowStartMs = 0;
static std::vector<String> g_multiCameraDigestCameras; // distinct camera names seen so far this window

// Records cfg.name into the current cross-camera alert window, opening a
// fresh one first if none is currently open. Called by triggerMotionAlert
// right after a real (non-quiet-hours, non-cooldown) send has already been
// decided - see both of its call sites below. A camera already present in
// the window (its own repeat alert, unlikely this close together given its
// own cooldown, but not impossible with a very short one configured) isn't
// added twice.
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

// English-only, lowercase label for the Activity log (event_log_store.h's
// text is always English regardless of TelegramLang - see
// telegram_i18n.h's own top comment for why) - "motion"/"person"/"vehicle"
// mirrors trMotionCaption's own Generic/Person/Vehicle distinction, so
// reviewing the Activity page can tell which kind of detection an alert
// actually was, not just that "a motion alert" happened. Previously every
// log line said "motion" regardless of kind, even after the Telegram
// caption itself started calling out PERSON/VEHICLE specifically.
static const char* motionKindLogLabel(MotionDetectionKind kind) {
  switch (kind) {
    case MotionDetectionKind::Person:  return "person";
    case MotionDetectionKind::Vehicle: return "vehicle";
    case MotionDetectionKind::Generic: return "motion";
  }
  return "motion"; // unreachable if every enumerator above is handled
}

// Maps a real motion/pet detection onto the stored-snapshot tag
// (snapshot_source.h) - a superset of MotionDetectionKind that also
// covers pet and every non-detection capture path (tamper, timelapse,
// test, manual) at their own pushCameraSnapshot call sites below.
static SnapshotSource snapshotSourceForMotion(bool isPetEvent, MotionDetectionKind kind) {
  if (isPetEvent) return SnapshotSource::Pet;
  switch (kind) {
    case MotionDetectionKind::Person:  return SnapshotSource::Person;
    case MotionDetectionKind::Vehicle: return SnapshotSource::Vehicle;
    case MotionDetectionKind::Generic: return SnapshotSource::Motion;
  }
  return SnapshotSource::Motion; // unreachable if every enumerator above is handled
}

// Feeds checkDailyActivityDigest's per-camera rollup below - called from
// every triggerMotionAlert point that just recorded a real, cooldown-
// clearing detection (quiet-hours-suppressed send, pet-text-only send,
// and the main real-send path). Caller must already hold st's
// CameraStateLock - all three call sites already do, for the
// lastAlert/hasAlerted write right alongside this.
static void incrementDigestCounter(CameraState& st, bool isPetEvent, MotionDetectionKind kind) {
  if (isPetEvent) { st.digestPetCount++; return; }
  switch (kind) {
    case MotionDetectionKind::Person:  st.digestPersonCount++;  return;
    case MotionDetectionKind::Vehicle: st.digestVehicleCount++; return;
    case MotionDetectionKind::Generic: st.digestMotionCount++;  return;
  }
}

void triggerMotionAlert(const CameraConfig& cfg, CameraState& st, bool isPetEvent, MotionDetectionKind kind) {
  // alertsEnabled is written by loop()'s task (pollTelegramCommands'
  // /on//off), this function runs on the camera's own task - cross-task
  // read, needs CameraStateLock. See CameraState::stateMutex.
  bool alertsEnabled;
  { CameraStateLock lock(st); alertsEnabled = st.alertsEnabled; }
  if (!alertsEnabled) return; // muted via Telegram - see pollTelegramCommands

  // hasAlerted/lastAlert are written only here, always from this same
  // task, so this self-read needs no lock - only cross-task readers
  // (webserver.cpp) do.
  uint32_t nowMs = millis();
  if (st.hasAlerted && nowMs - st.lastAlert < safeAlertCooldownMs(cfg)) {
    // Only counted while a real snapshot send (below) is what started this
    // cooldown - digestArmed stays false through a quiet-hours cycle (see
    // that branch below), so motion suppressed during a quiet stretch
    // doesn't get reported as "since the snapshot" when no snapshot was
    // actually sent to anyone. See checkPendingMotionDigest for the flush.
    // A suppressed PET event is deliberately never counted here - this
    // counter's own contract (CameraState::suppressedMotionCount) and the
    // digest message it feeds ("motion continued - N more event(s)") are
    // both specifically about motion, and a cat wandering by during the
    // cooldown isn't what either one is claiming happened. It's simply
    // dropped, silently, consistent with pet alerts being the
    // lower-priority notification type by design.
    if (st.digestArmed && !isPetEvent) {
      st.suppressedMotionCount++;
      st.lastSuppressedMotionMs = nowMs; // when THIS event happened, not when the digest will flush
    }
    return; // cooling down
  }

  // Quiet hours suppresses the Telegram send only - motion is still
  // detected/cooldown-gated/recorded (Activity log + snapshot history),
  // just doesn't page anyone. Deliberately scoped to motion alerts only -
  // triggerTamperAlert/triggerSignalLossAlert stay always-on (security/
  // connectivity-critical, not "noise").
  bool quiet = cfg.quietHoursEnabled && localClockSynced() &&
               isWithinQuietHours(currentLocalMinuteOfDay(), cfg.quietStartMinute, cfg.quietEndMinute);
  if (quiet) {
    // Mirrors this function's own "don't spend the cooldown on a no-op"
    // rule below - nothing to capture yet if the snapshot URI hasn't
    // resolved, so don't burn the cooldown window on nothing.
    if (st.snapshotUri.length() == 0) return;
    // digestArmed/suppressedMotionCount MUST be cleared here, not just
    // left alone - this branch resets lastAlert same as a real send does,
    // but unlike a real send it's the ONLY other place lastAlert moves.
    // Without this, a digest already armed by an earlier real alert (still
    // mid-cooldown, with a nonzero suppressedMotionCount) would survive
    // into this new cycle with its old count and old lastSuppressedMotionMs
    // intact, while lastAlert jumps to nowMs - checkPendingMotionDigest's
    // later flush then computes lastSuppressedMotionMs - st.lastAlert with
    // a lastSuppressedMotionMs from BEFORE the new (larger) lastAlert,
    // underflowing that unsigned subtraction into a nonsensical multi-
    // billion-second digest message. The comment below this function's
    // cooldown check ("digestArmed stays false through a quiet-hours
    // cycle") was already assuming this - it just wasn't actually enforced.
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

  // Pet alert, text-only mode: no photo in the Telegram message itself,
  // but the snapshot is still captured and pushed to history same as every
  // other alert path here (triggerTimelapseCapture always stores
  // regardless of its own Telegram opt-in; a regular motion alert during
  // quiet hours always stores despite suppressing the send) - "text only"
  // means the message, not "never capture a photo of this at all." None of
  // the burst-loop/latency-tracking machinery below applies since there's
  // only ever one shot and no photo to attach.
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

  // snapshotUri is written only by this camera's own task too (camera.cpp),
  // same-task self-read, no lock needed here either.
  if (st.snapshotUri.length() == 0) {
    Serial.printf("[%s] No snapshot URI available - skipping Telegram send.\n", cfg.name.c_str());
    return;
  }

  // Cooldown is only spent once a send is actually attempted (past this
  // point) - marking it earlier would let a camera with no subscribers, or
  // an unresolved snapshot URI, silently burn every motion event's cooldown
  // doing nothing. A failure past this point still spends it, on purpose,
  // to stop sustained motion from retry-storming a misbehaving camera.
  { CameraStateLock lock(st); st.lastAlert = nowMs; st.hasAlerted = true;
    incrementDigestCounter(st, isPetEvent, kind); }
  // Starts a fresh digest cycle for checkPendingMotionDigest - see
  // suppressedMotionCount's own comment (camera.h). Reset here, not just
  // left to accumulate, so a digest never double-counts events already
  // reported by a previous cycle's flush.
  st.digestArmed = true;
  st.suppressedMotionCount = 0;

  unsigned int shots = safeSnapshotBurstCount(cfg);
  logEvent(cfg.name + ": " + (isPetEvent ? "pet" : motionKindLogLabel(kind)) + " alert, " + String(shots) +
           " shot(s) to " + String(recipients.size()) + " recipient(s)");
  noteMultiCameraAlert(cfg.name);

  // Each shot is its own fetch (re-fetching is what makes consecutive
  // shots differ) and its own fan-out to every recipient - re-fetching per
  // recipient instead would hammer cameras whose HTTP stacks only tolerate
  // 1-2 connections. No artificial delay between shots: the fetch and
  // upload each take real time on their own, already enough spacing.
  for (unsigned int i = 0; i < shots; i++) {
    size_t jpgLen = 0;
    uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
    if (i == 0) {
      // How long from the real motion signal (camera.cpp's parseEvents
      // setting lastMotionMs, independent of the cooldown/quiet-hours
      // gating above) to this first photo actually being in hand - lets
      // tuning CameraConfig::pollIntervalMs be judged by an actual number
      // instead of eyeballing whether photos "look late." Same-task-only
      // read of lastMotionMs, no lock needed - see its own comment
      // (camera.h). Remove this if it turns out to be more log noise than
      // it's worth - it's diagnostic, nothing else depends on it.
      unsigned long latencyMs = millis() - st.lastMotionMs;
      logEvent(cfg.name + ": " + String(latencyMs) +
               "ms from motion to first photo" + (jpg ? "" : " (fetch failed)"));
      if (jpg) {
        // Only recorded on an actual successful fetch - see
        // CameraState::motionLatencyHistory's own comment (camera.h) for
        // why a failed fetch's latency would skew the rollup.
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

// Manual "Send test alert" button (Cameras dashboard page,
// webserver_cameras.cpp's startTestAlertAsync). Fetches ONE fresh
// snapshot and sends it, clearly captioned as a test (trTestAlertCaption),
// to every Telegram user currently subscribed to this camera
// (telegramUserWantsCamera) - the same recipient list a real motion alert
// would use, so the whole snapshot-fetch/recipient-filtering/Telegram-
// delivery chain gets verified end-to-end, without waiting for real
// motion or faking a PIR trip. Deliberately ignores st.alertsEnabled
// (muted or not), quiet hours, and the alert cooldown entirely, and does
// NOT touch st.lastAlert/hasAlerted/digestArmed/suppressedMotionCount - a
// manual test must never interfere with the real motion-alert state
// machine, same "manual override, real state machine left alone"
// reasoning as net_watchdog.h's manualPulseRelay. Still pushes the
// snapshot to this camera's history (pushCameraSnapshot) same as every
// other alert path, so it shows up in the Preview column too.
// `outDetail` is set to a human-readable reason on any failure (left
// untouched on success).
bool sendTestAlert(const CameraConfig& cfg, CameraState& st, String& outDetail, MotionDetectionKind kind,
                    bool isPetEvent) {
  // snapshotUri is written by this camera's own task (camera.cpp) but this
  // runs from the Send-Test-Alert background task - cross-task read,
  // needs CameraStateLock. See CameraState::stateMutex.
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
  // snapshotUri is written only by this camera's own task (camera.cpp),
  // same-task self-read, no lock needed - same reasoning as
  // triggerMotionAlert's own check.
  if (st.snapshotUri.length() == 0) return;

  size_t jpgLen = 0;
  uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
  if (!jpg) return; // logged by fetchOneSnapshot itself

  // Opt-in delivery to Telegram - most cameras with a timelapse interval
  // just want it stored (the original, still-default behavior below).
  // Deliberately does NOT go through beginCameraAlert (unlike
  // triggerTamperAlert/triggerSignalLossAlert): that helper's cooldown/
  // hasAlerted bookkeeping is motion-alert throttling state, unrelated to
  // (and must not be perturbed by) a schedule this feature already owns
  // via its own interval - a timelapse capture should send every time its
  // own timer fires, not be throttled or recorded as if it were a motion
  // alert. Still respects the mute toggle (st.alertsEnabled): muting a
  // camera is a blanket "no messages from this camera" expectation, not
  // motion-alerts-only.
  // Same quiet-hours scoping as triggerMotionAlert above: suppresses the
  // Telegram send only, not the capture/storage itself (still pushed to
  // history below regardless) - a routine scheduled snapshot at 3am is, if
  // anything, a worse fit for quiet hours than a real motion event would
  // be, since it isn't even responding to anything happening.
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
      // Deliberately distinct wording from a motion-alert caption (no
      // warning glyph, "scheduled" spelled out) - a recipient should never
      // mistake a routine timelapse photo for something that needs
      // attention.
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

// Gathers this camera's subscribed recipients and, if there are any and
// the cooldown has cleared, spends it (lastAlert/hasAlerted) and returns
// them - shared by triggerTamperAlert/triggerSignalLossAlert below.
// Returns empty (spending nothing) if muted, cooling down, or nobody's
// subscribed. triggerMotionAlert doesn't use this: it has one more gate
// (snapshotUri resolved) before the cooldown should be spent, which
// tamper/signal-loss don't share (tamper degrades to text-only,
// signal-loss is always text-only) - not unified into one helper to avoid
// forcing that extra gate onto events that don't need it.
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
    // No snapshot URI yet, or the fetch itself failed - tamper is
    // important enough not to stay silent just because a photo isn't
    // available right now.
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
  // lastContactMs is lock-guarded (see cameraSoapCall's/pushCameraSnapshot's
  // own comments) - pushCameraSnapshot can adjust it from loop()'s task
  // (sendOnDemandSnapshot, via /snap), not just this camera's own task.
  unsigned long lastContactMs;
  { CameraStateLock lock(st); lastContactMs = st.lastContactMs; }
  unsigned long offlineThresholdMs = safeOfflineThresholdMs(cfg);
  bool offlineNow = (millis() - lastContactMs) >= offlineThresholdMs;
  // isOffline is written only here, always from this camera's own task, so
  // this self-read needs no lock - only the write below does, since
  // webserver.cpp/main.cpp read it from other tasks.
  if (offlineNow == st.isOffline) return; // no state change - most calls hit this

  {
    CameraStateLock lock(st);
    st.isOffline = offlineNow;
    if (offlineNow) {
      // Pushed only on the false->true transition, not every check - see
      // CameraState::offlineHistory's own comment (camera.h).
      st.offlineHistory[st.offlineHistoryNext] = millis();
      st.offlineHistoryNext = (st.offlineHistoryNext + 1) % EVENT_HISTORY_RING_SIZE;
      if (st.offlineHistoryCount < EVENT_HISTORY_RING_SIZE) st.offlineHistoryCount++;
    }
  }
  if (offlineNow) {
    // Reports the same (possibly clamped) threshold actually enforced above,
    // not the raw cfg field - otherwise a value clamped by
    // safeOfflineThresholdMs would make this message describe a threshold
    // that was never really in effect.
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

// lastSubscribedMs/subscriptionLostAlerted are same-task-only (see
// CameraState's own comment) - this runs on the camera's own task, same as
// checkCameraOnlineStatus above, no lock needed. isOffline itself is a
// same-task self-read here too - see checkCameraOnlineStatus's own comment.
void checkSubscriptionHealth(const CameraConfig& cfg, CameraState& st) {
  // Reuses the same (already-clamped) threshold checkCameraOnlineStatus
  // does - "hasn't been able to hold a subscription for as long as it
  // would take to be considered OFFLINE" is exactly the bar this is meant
  // to catch, without adding a whole separate dashboard field for it. The
  // actual alert-once/re-arm decision is pure logic (subscription_health.h),
  // natively tested (test/test_subscription_health) - this function is just
  // the FreeRTOS/CameraState/Telegram glue around it.
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

// lastMotionMs/motionWatchdogTripped are same-task-only (see CameraState's
// own comment) - this runs on the camera's own task, same as
// checkCameraOnlineStatus above, no lock needed.
void checkMotionWatchdog(const CameraConfig& cfg, CameraState& st) {
  if (cfg.motionWatchdogHours == 0) return; // disabled

  // Clamped here, at the point of use, not just at the dashboard form
  // (webserver_cameras.cpp clamps to [0,168], but cfg.motionWatchdogHours
  // is a uint16_t that could in principle hold up to 65535 via a hand-
  // edited/imported NVS blob that bypasses the form entirely). Real bound,
  // not a sanity nicety: unsigned long is 32-bit on this platform, and
  // hours * 3600000UL overflows/wraps above ~1193 hours - an
  // absurdly-large configured value would silently wrap into a SMALL
  // threshold instead of a large one, tripping the watchdog almost
  // immediately instead of almost never, the opposite of what such a
  // value would be trying to express.
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

// Flushes triggerMotionAlert's suppressedMotionCount as one summary text
// once the cooldown it accumulated during ends - so "5 more motion events
// in the last 30 seconds" (motion kept happening) and silence (it was a
// one-off) are both visible, instead of every event after the first
// simply vanishing until the next real send. digestArmed/
// suppressedMotionCount are same-task-only (see their own comments,
// camera.h), same reasoning as checkMotionWatchdog above - no lock needed.
void checkPendingMotionDigest(const CameraConfig& cfg, CameraState& st) {
  if (!st.digestArmed) return; // no real send is currently being tracked
  if (millis() - st.lastAlert < safeAlertCooldownMs(cfg)) return; // cooldown still running - not due yet

  st.digestArmed = false; // one flush per cooldown cycle, whatever the count
  uint32_t count = st.suppressedMotionCount;
  unsigned long lastSuppressedMotionMs = st.lastSuppressedMotionMs;
  st.suppressedMotionCount = 0;
  if (count == 0) return; // genuinely a one-off - nothing to report
  // Bookkeeping above (digestArmed/suppressedMotionCount reset) always
  // happens regardless of this setting - only the SEND is skipped, so
  // toggling it live never leaves stale state for the next cooldown cycle
  // to trip over.
  if (!cfg.motionDigestEnabled) return;

  // alertsEnabled is written cross-task (pollTelegramCommands' /on//off) -
  // needs the lock, same as triggerMotionAlert's own read of it.
  bool alertsEnabled;
  { CameraStateLock lock(st); alertsEnabled = st.alertsEnabled; }
  if (!alertsEnabled) return; // muted since the snapshot went out - stay quiet

  std::vector<AlertRecipient> recipients;
  for (auto& u : loadTelegramUsers()) {
    if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back({u.chatId, u.language});
  }
  if (recipients.empty()) return;

  // Seconds, not a vaguer "since the last snapshot" - matches how a user
  // sets alertCooldownSec on the Cameras page, so it's a unit they'll
  // actually recognize. Measured from the snapshot to the LAST suppressed
  // event, not to now (when the cooldown/digest happens to flush) - motion
  // that actually stopped 10s into a 30s cooldown should say "10 seconds,"
  // not "30 seconds" just because that's when this check next ran.
  unsigned long elapsedSec = (lastSuppressedMotionMs - st.lastAlert) / 1000UL;
  for (auto& r : recipients) {
    sendTelegramMessageTo(r.chatId, trMotionDigest(r.language, cfg.name, count, elapsedSec));
  }
  logEvent(cfg.name + ": motion digest - " + String(count) + " event(s) in " + String(elapsedSec) + "s");
}

// See its own header comment (telegram.h) and noteMultiCameraAlert's
// (above) for the full design. Reads/clears the shared window under
// g_multiCameraDigestMutex, then does the actual (possibly slow) Telegram
// send OUTSIDE the lock - same "don't hold a mutex across a blocking
// network call" discipline as every other cross-task lock in this file.
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
  // A single camera's own burst is exactly what its per-camera cooldown/
  // digest (checkPendingMotionDigest above) already reports - no
  // cross-camera correlation to add here, so silently discard the window
  // rather than sending a one-camera "digest" that says nothing new.
  if (cameras.size() < 2) return;

  String cameraList;
  for (size_t i = 0; i < cameras.size(); i++) cameraList += (i > 0 ? ", " : "") + cameras[i];
  sendTelegramMessage([&](TelegramLang lang) {
    return trMultiCameraDigest(lang, (uint32_t)cameras.size(), cameraList);
  });
  logEvent(String((unsigned)cameras.size()) + " cameras detected motion together: " + cameraList);
}

// One camera's tally for checkDailyActivityDigest below - only built for a
// camera that actually had at least one non-zero count, so an idle
// property with a handful of quiet cameras doesn't produce a wall of
// "no activity" lines.
struct DailyDigestTally {
  String name;
  uint32_t person, vehicle, pet, motion;
};

void checkDailyActivityDigest(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  std::vector<DailyDigestTally> tallies;
  for (size_t i = 0; i < numCameras; i++) {
    uint32_t person, vehicle, pet, motion;
    {
      // Read AND reset under the same lock - this is the one place these
      // four counters are ever cleared (incrementDigestCounter above only
      // ever adds to them), so every camera's counting window restarts
      // together, right here, regardless of whether it had any activity
      // this cycle.
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

