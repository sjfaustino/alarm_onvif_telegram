#include "telegram.h"
#include "telegram_ca.h"
#include "telegram_users.h"
#include "telegram_parse.h"
#include "telegram_multipart.h"
#include "format_utils.h"
#include "event_log_store.h"
#include "snapshot_history.h"
#include "sd_store.h"
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <NetworkClient.h> // HTTPClient::getStreamPtr() returns NetworkClient* on Arduino-ESP32 3.x cores
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <Preferences.h>
#include <time.h>
#include <cstring>
#include <vector>

bool telegramCAConfigured() {
  // Sanity check that this actually looks like a PEM certificate, not a
  // placeholder/filename/empty string left behind by mistake.
  return strstr(TELEGRAM_ROOT_CA, "-----BEGIN CERTIFICATE-----") != nullptr &&
         strstr(TELEGRAM_ROOT_CA, "-----END CERTIFICATE-----") != nullptr;
}

// ============================================================
// Camera -> Telegram
//
// PSRAM is a hard requirement (main.cpp's setup() refuses to boot without
// it): a motion alert can go to more than one Telegram user, so the JPEG
// is fetched from the camera once, held in memory, and resent per
// recipient (see triggerMotionAlert below). SNAPSHOT_MAX_BYTES (much
// smaller than SNAPSHOT_MAX_BYTES_PSRAM) survives only as
// allocateSnapshotBuffer's fallback cap if a PSRAM allocation itself fails.
// ============================================================

// Allocates `cap` bytes for a snapshot buffer, preferring PSRAM and
// falling back to internal RAM only if that allocation itself fails.
static uint8_t* allocateSnapshotBuffer(size_t cap) {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)malloc(cap);
  return buf;
}

// WiFiClientSecure::write() can do a partial write under TLS, especially on
// memory-constrained boards. This loops until every byte is confirmed
// written, the connection drops, or it stalls for 5s with no progress.
static size_t writeAllBytes(WiFiClientSecure& client, const uint8_t* data, size_t len) {
  size_t written = 0;
  uint32_t stallStart = millis();
  while (written < len) {
    if (!client.connected()) {
      Serial.println("writeAllBytes: connection dropped mid-write");
      break;
    }
    size_t n = client.write(data + written, len - written);
    if (n > 0) {
      written += n;
      stallStart = millis();
    } else if (millis() - stallStart > 5000) {
      Serial.println("writeAllBytes: stalled, no progress for 5s");
      break;
    } else {
      delay(2);
    }
  }
  return written;
}

// Reads up to `want` bytes from the camera's HTTP stream into buf, using the
// same "wait for data, bail on stall" pattern as before. Returns bytes
// actually read - only less than `want` if the stream ended or stalled.
static size_t readSomeBytes(HTTPClient& http, NetworkClient* stream, uint8_t* buf, size_t want) {
  size_t total = 0;
  uint32_t stallStart = millis();
  while (http.connected() && total < want) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = min(avail, want - total);
      total += stream->readBytes(buf + total, toRead);
      stallStart = millis();
    } else if (millis() - stallStart > 5000) {
      Serial.println("readSomeBytes: stalled, no progress for 5s");
      break;
    } else {
      delay(2);
    }
  }
  return total;
}

// Buffers a snapshot fully (into PSRAM if available, else internal RAM -
// see allocateSnapshotBuffer above) up to `cap` bytes. Used both as the
// primary path on PSRAM boards and as the no-Content-Length fallback on
// non-PSRAM boards. Caller must free() the returned buffer.
static uint8_t* fetchSnapshotBuffered(HTTPClient& http, size_t& outLen, size_t cap) {
  outLen = 0;
  uint8_t* buf = allocateSnapshotBuffer(cap);
  if (!buf) {
    Serial.println("Snapshot buffer allocation failed.");
    return nullptr;
  }
  size_t total = readSomeBytes(http, http.getStreamPtr(), buf, cap);
  if (total == 0) {
    free(buf);
    return nullptr;
  }
  outLen = total;
  return buf;
}

static bool readTelegramResponse(WiFiClientSecure& client) {
  uint32_t t0 = millis();
  while (client.connected() && !client.available() && millis() - t0 < 10000) delay(10);
  if (!client.available()) {
    Serial.println("Telegram sendPhoto: no response within timeout.");
    client.stop();
    return false;
  }

  String fullResponse;
  uint32_t readStart = millis();
  while (client.connected() && millis() - readStart < 3000) {
    while (client.available()) { fullResponse += (char)client.read(); readStart = millis(); }
  }
  while (client.available()) fullResponse += (char)client.read();
  client.stop();

  String statusLine = fullResponse.substring(0, fullResponse.indexOf('\n'));
  bool ok = statusLine.indexOf("200") > 0;
  Serial.println(ok ? "Telegram sendPhoto OK" : "Telegram sendPhoto FAILED: " + statusLine);
  return ok;
}

// Sends a buffer that's already fully in RAM to one chat - called once per
// subscribed Telegram user, reusing the same PSRAM buffer (see
// triggerMotionAlert).
static bool sendTelegramPhotoBuffered(const uint8_t* jpg, size_t jpgLen, const String& caption,
                                       const String& chatId) {
  if (!jpg || jpgLen == 0) return false;

  Serial.printf("Free heap before Telegram send: %u bytes (max alloc: %u, jpg: %u bytes)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)jpgLen);

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  if (!client.connect("api.telegram.org", 443)) {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("Could not connect to api.telegram.org - TLS/socket error: %s\n", errBuf);
    if (!telegramCAConfigured()) {
      Serial.println("  ^ TELEGRAM_ROOT_CA in telegram_ca.h is still the placeholder - fill it in.");
    }
    Serial.printf("Free heap at failure: %u bytes\n", (unsigned)ESP.getFreeHeap());
    return false;
  }

  TelegramMultipart m = buildMultipart(jpgLen, caption, chatId, TELEGRAM_BOT_TOKEN);

  size_t sent = 0;
  sent += writeAllBytes(client, (const uint8_t*)m.requestLine.c_str(), m.requestLine.length());
  sent += writeAllBytes(client, (const uint8_t*)m.head.c_str(), m.head.length());
  sent += writeAllBytes(client, jpg, jpgLen);
  sent += writeAllBytes(client, (const uint8_t*)m.tail.c_str(), m.tail.length());

  size_t expectedTotal = m.requestLine.length() + m.contentLength;
  if (sent < expectedTotal) {
    Serial.println("Write incomplete - server will never see the full request.");
    client.stop();
    return false;
  }

  return readTelegramResponse(client);
}

// Retries once (fresh TLS connection) on failure. A failed/stalled write
// (see writeAllBytes' comment) doesn't necessarily mean the network is
// unusable - concurrent camera polling contending for the one WiFi radio
// has been observed to stall a large photo upload until mbedTLS gives up
// and closes the connection; a second attempt often lands in a quieter
// moment.
static bool sendTelegramPhotoWithRetry(const uint8_t* jpg, size_t jpgLen, const String& caption,
                                        const String& chatId) {
  static const int MAX_ATTEMPTS = 2;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    if (sendTelegramPhotoBuffered(jpg, jpgLen, caption, chatId)) return true;
    if (attempt < MAX_ATTEMPTS) {
      Serial.printf("Telegram photo send to chat %s failed (attempt %d/%d) - retrying.\n",
                    chatId.c_str(), attempt, MAX_ATTEMPTS);
    }
  }
  return false;
}

// localtime_r, not gmtime_r - honors whatever POSIX TZ rule main.cpp's
// setupTime() applied at boot (WifiCredentials::posixTz), or plain UTC if
// none configured. The system clock itself always stays true UTC either
// way - only this *display* value is affected; WS-Security's timestamp
// reads UTC directly via gmtime_r() and is unaffected by TZ.
static String nowTimestampString() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmStruct);
  return String(buf);
}

// Low-level single-recipient send (JSON body, no photo) - the actual HTTPS
// round trip. Used both directly (command replies go back to whoever sent
// the command, not everyone) and by sendTelegramMessage() below (broadcast).
//
// Uses HTTPClient rather than a raw WiFiClientSecure + hand-built request
// line (unlike sendTelegramPhotoBuffered, which streams a multipart body
// HTTPClient has no API for) - HTTPClient correctly handles chunked
// transfer-encoding on the response, which a manual "read until idle, split
// on the first \r\n\r\n" parser doesn't; api.telegram.org is free to send
// a chunked response and has been observed to.
static bool sendTelegramMessageTo(const String& chatId, const String& text) {
  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage";
  if (!http.begin(client, url)) {
    Serial.printf("sendTelegramMessageTo(%s): http.begin() failed.\n", chatId.c_str());
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  JsonDocument outDoc;
  outDoc["chat_id"] = chatId;
  outDoc["text"] = text;
  String body;
  serializeJson(outDoc, body);

  int code = http.POST(body);
  bool ok = (code == 200);
  if (!ok) {
    Serial.printf("sendTelegramMessageTo(%s): HTTP %d", chatId.c_str(), code);
    if (code > 0) {
      Serial.printf(" - %s\n", http.getString().c_str());
    } else {
      Serial.printf(" - %s\n", HTTPClient::errorToString(code).c_str());
      if (!telegramCAConfigured()) {
        Serial.println("  ^ TELEGRAM_ROOT_CA in telegram_ca.h is still the placeholder - fill it in.");
      }
    }
  }
  http.end();
  return ok;
}

// Broadcasts to every Telegram user with systemMessages enabled - used for
// the heartbeat, the boot-online notice, and the "no credentials" fatal
// alert. Returns true if it reached at least one recipient.
bool sendTelegramMessage(const String& text) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  bool anyRecipient = false;
  bool anyOk = false;
  for (auto& u : users) {
    if (!u.systemMessages) continue;
    anyRecipient = true;
    if (sendTelegramMessageTo(u.chatId, text)) anyOk = true;
  }
  if (!anyRecipient) {
    Serial.println("sendTelegramMessage: no Telegram user has systemMessages enabled - nothing sent.");
    return false;
  }
  return anyOk;
}

// Fetches exactly one snapshot from st.snapshotUri, buffered fully (see
// fetchSnapshotBuffered). Returns nullptr (and logs) on any failure - HTTP
// GET failure, or the buffer fetch itself failing. Caller must free() a
// non-null result.
static uint8_t* fetchOneSnapshot(const CameraConfig& cfg, CameraState& st, size_t& outLen) {
  outLen = 0;

  // snapshotUri/user/pass are written by this camera's own task
  // (camera.cpp) but this function is also called from loop()'s task via
  // /snap - copy them out under CameraStateLock rather than reading the
  // live fields directly. See CameraState::stateMutex.
  String snapshotUri; const char* user; const char* pass;
  {
    CameraStateLock lock(st);
    snapshotUri = st.snapshotUri;
    user = st.user;
    pass = st.pass;
  }

  HTTPClient http;
  http.begin(snapshotUri);
  http.setAuthorization(user, pass);
  http.setTimeout(HTTP_TIMEOUT_MS);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[%s] Snapshot GET failed, HTTP %d\n", cfg.name.c_str(), code);
    http.end();
    return nullptr;
  }

  int len = http.getSize();
  // Read exactly `len` bytes when the camera reports Content-Length, so
  // readSomeBytes stops as soon as the file is fully read instead of
  // reading toward the full cap and eating a 5s stall-timeout on a
  // keep-alive connection with no more data to send (confirmed slow enough
  // on one camera - a Reolink cgi-bin - to make the Telegram send that
  // followed fail outright).
  size_t cap = (len > 0) ? (size_t)len : SNAPSHOT_MAX_BYTES_PSRAM;

  size_t jpgLen = 0;
  uint8_t* jpg = fetchSnapshotBuffered(http, jpgLen, cap);
  http.end();
  if (!jpg) {
    Serial.printf("[%s] Snapshot fetch failed.\n", cfg.name.c_str());
    return nullptr;
  }
  outLen = jpgLen;
  return jpg;
}

void triggerMotionAlert(const CameraConfig& cfg, CameraState& st) {
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
  if (st.hasAlerted && nowMs - st.lastAlert < cfg.alertCooldownMs) return; // cooling down

  std::vector<String> recipients;
  for (auto& u : loadTelegramUsers()) {
    if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back(u.chatId);
  }
  if (recipients.empty()) {
    Serial.printf("[%s] No Telegram user is subscribed to this camera - skipping send.\n", cfg.name.c_str());
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
  { CameraStateLock lock(st); st.lastAlert = nowMs; st.hasAlerted = true; }

  unsigned int shots = (cfg.snapshotBurstCount > 0) ? cfg.snapshotBurstCount : 1;
  logEvent(cfg.name + ": motion alert, " + String(shots) + " shot(s) to " +
           String(recipients.size()) + " recipient(s)");

  // Each shot is its own fetch (re-fetching is what makes consecutive
  // shots differ) and its own fan-out to every recipient - re-fetching per
  // recipient instead would hammer cameras whose HTTP stacks only tolerate
  // 1-2 connections. No artificial delay between shots: the fetch and
  // upload each take real time on their own, already enough spacing.
  for (unsigned int i = 0; i < shots; i++) {
    size_t jpgLen = 0;
    uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
    if (!jpg) continue; // one bad shot in a burst shouldn't abort the rest

    String caption = cfg.name + " - " + nowTimestampString();
    if (shots > 1) caption += " (" + String(i + 1) + "/" + String(shots) + ")";

    for (auto& chatId : recipients) {
      if (!sendTelegramPhotoWithRetry(jpg, jpgLen, caption, chatId)) {
        Serial.printf("[%s] Telegram send to chat %s failed.\n", cfg.name.c_str(), chatId.c_str());
      }
    }
    pushCameraSnapshot(cfg, st, jpg, jpgLen); // takes ownership - do not free(jpg) here
  }
}

// Gathers this camera's subscribed recipients and, if there are any and
// the cooldown has cleared, spends it (lastAlert/hasAlerted) and returns
// them - shared by triggerTamperAlert/triggerSignalLossAlert below.
// Returns empty (and spends nothing) if muted, still cooling down, or
// nobody's subscribed - same "don't burn the cooldown on nothing" rule
// triggerMotionAlert documents, though that function doesn't use this
// helper itself: it has one more gate (snapshotUri must be resolved)
// before the cooldown should be spent, which tamper/signal-loss alerts
// don't share (tamper degrades to text-only, signal-loss is always
// text-only), so unifying all three into one helper would mean forcing
// motion's extra gate onto events that don't need it, or forcing this
// simpler version's ordering onto motion and losing its "no snapshot URI
// yet" pre-cooldown check.
static std::vector<String> beginCameraAlert(const CameraConfig& cfg, CameraState& st, uint32_t nowMs) {
  bool alertsEnabled;
  { CameraStateLock lock(st); alertsEnabled = st.alertsEnabled; }
  if (!alertsEnabled) return {};

  if (st.hasAlerted && nowMs - st.lastAlert < cfg.alertCooldownMs) return {};

  std::vector<String> recipients;
  for (auto& u : loadTelegramUsers()) {
    if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back(u.chatId);
  }
  if (recipients.empty()) return {};

  { CameraStateLock lock(st); st.lastAlert = nowMs; st.hasAlerted = true; }
  return recipients;
}

void triggerTamperAlert(const CameraConfig& cfg, CameraState& st) {
  std::vector<String> recipients = beginCameraAlert(cfg, st, millis());
  if (recipients.empty()) return;

  logEvent(cfg.name + ": TAMPER detected");
  String caption = "\xE2\x9A\xA0\xEF\xB8\x8F " + cfg.name + " - TAMPER DETECTED - " + nowTimestampString();

  bool hasSnapshotUri;
  { CameraStateLock lock(st); hasSnapshotUri = st.snapshotUri.length() > 0; }
  size_t jpgLen = 0;
  uint8_t* jpg = hasSnapshotUri ? fetchOneSnapshot(cfg, st, jpgLen) : nullptr;

  if (jpg) {
    for (auto& chatId : recipients) {
      if (!sendTelegramPhotoWithRetry(jpg, jpgLen, caption, chatId)) {
        Serial.printf("[%s] Tamper alert photo send to chat %s failed.\n", cfg.name.c_str(), chatId.c_str());
      }
    }
    pushCameraSnapshot(cfg, st, jpg, jpgLen); // takes ownership - do not free(jpg) here
  } else {
    // No snapshot URI yet, or the fetch itself failed - tamper is
    // important enough not to stay silent just because a photo isn't
    // available right now.
    for (auto& chatId : recipients) sendTelegramMessageTo(chatId, caption);
  }
}

void triggerSignalLossAlert(const CameraConfig& cfg, CameraState& st) {
  std::vector<String> recipients = beginCameraAlert(cfg, st, millis());
  if (recipients.empty()) return;

  logEvent(cfg.name + ": video SIGNAL LOSS");
  String msg = "\xE2\x9A\xA0\xEF\xB8\x8F " + cfg.name + " - VIDEO SIGNAL LOSS - " + nowTimestampString();
  for (auto& chatId : recipients) sendTelegramMessageTo(chatId, msg);
}

void checkCameraOnlineStatus(const CameraConfig& cfg, CameraState& st) {
  bool offlineNow = (millis() - st.lastContactMs) >= cfg.offlineThresholdMs;
  // isOffline is written only here, always from this camera's own task, so
  // this self-read needs no lock - only the write below does, since
  // webserver.cpp/main.cpp read it from other tasks.
  if (offlineNow == st.isOffline) return; // no state change - most calls hit this

  { CameraStateLock lock(st); st.isOffline = offlineNow; }
  if (offlineNow) {
    Serial.printf("[%s] OFFLINE - no response for over %lus.\n", cfg.name.c_str(), cfg.offlineThresholdMs / 1000UL);
    logEvent(cfg.name + ": OFFLINE (no response for over " + String(cfg.offlineThresholdMs / 60000UL) + "m)");
    sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F " + cfg.name + " is OFFLINE - no response for over " +
                         String(cfg.offlineThresholdMs / 60000UL) + " minute(s).");
  } else {
    Serial.printf("[%s] Back ONLINE.\n", cfg.name.c_str());
    logEvent(cfg.name + ": back ONLINE");
    sendTelegramMessage("\xE2\x9C\x85 " + cfg.name + " is back ONLINE.");
  }
}

// ============================================================
// Remote on/off control (Telegram commands)
//
// pollTelegramCommands() is called periodically from loop() and does a
// short (timeout=0, not long-poll) getUpdates round trip. lastUpdateId is
// persisted in NVS (see loadLastUpdateId/saveLastUpdateId below) - it used
// not to be, on the theory that redelivering a couple of already-applied
// commands after a reboot is harmless since /on/off/snap/status/uptime are
// idempotent. /reset broke that assumption: redelivering it after the
// reboot it itself caused re-executes /reset again, forever - an infinite
// reboot loop hit in the field the very first time /reset was used.
//
// Persisting on every single update (the first fix) closed that loop but
// opened a smaller one: Telegram delivers every inbound message to
// getUpdates regardless of who sent it, so an unauthenticated flood of
// messages would have forced an NVS write per message with zero
// permission check. pollTelegramCommands() below persists once per poll
// instead (bounded, matches the original harmless-redelivery assumption
// for everything except /reset).
//
// /reset is the one command that can't wait for that end-of-poll persist -
// ESP.restart() never returns, so the code would never get back around to
// it. That's handled inside handleTelegramCommand's own /reset branch, not
// guessed at here: pollTelegramCommands has no special-cased knowledge of
// which commands are "dangerous" (an earlier version tried that, checking
// canReset and the command text here before dispatching - two places
// having to agree on what /reset is turned out to be exactly the kind of
// thing that drifts out of sync). handleTelegramCommand persists
// immediately, right before the action that actually needs it, which is
// also the right place for any future command that needs the same thing.
// ============================================================

static const char* TELEGRAM_STATE_NAMESPACE = "tgstate";
static const char* TELEGRAM_STATE_KEY_LAST_UPDATE_ID = "lastUpdateId";

static long loadLastUpdateId() {
  Preferences prefs;
  prefs.begin(TELEGRAM_STATE_NAMESPACE, true); // read-only
  long id = prefs.getLong(TELEGRAM_STATE_KEY_LAST_UPDATE_ID, 0);
  prefs.end();
  return id;
}

static void saveLastUpdateId(long id) {
  Preferences prefs;
  if (prefs.begin(TELEGRAM_STATE_NAMESPACE, false)) {
    prefs.putLong(TELEGRAM_STATE_KEY_LAST_UPDATE_ID, id);
    prefs.end();
  }
}

static const char* ALERT_PREF_NAMESPACE = "camctl";

bool loadAlertEnabledPref(size_t index) {
  Preferences prefs;
  prefs.begin(ALERT_PREF_NAMESPACE, true); // read-only
  char key[8];
  snprintf(key, sizeof(key), "c%u", (unsigned)index);
  bool enabled = prefs.getBool(key, true); // default ON if never set
  prefs.end();
  return enabled;
}

static void saveAlertEnabledPref(size_t index, bool enabled) {
  Preferences prefs;
  prefs.begin(ALERT_PREF_NAMESPACE, false);
  char key[8];
  snprintf(key, sizeof(key), "c%u", (unsigned)index);
  prefs.putBool(key, enabled);
  prefs.end();
}

// Fetches a fresh snapshot from cfg/st right now and sends it only to
// chatId (whoever asked) - unlike triggerMotionAlert, this is an explicit
// one-off request, not a motion alert, so it ignores st.alertsEnabled and
// doesn't touch st.lastAlert/hasAlerted or spend the alert cooldown.
static void sendOnDemandSnapshot(const CameraConfig& cfg, CameraState& st, const String& chatId) {
  // This runs on loop()'s task, snapshotUri is written by the camera's own
  // task - cross-task read, needs CameraStateLock. See CameraState::stateMutex.
  bool hasSnapshotUri;
  { CameraStateLock lock(st); hasSnapshotUri = st.snapshotUri.length() > 0; }
  if (!hasSnapshotUri) {
    sendTelegramMessageTo(chatId, cfg.name + ": no snapshot URI available yet.");
    return;
  }

  size_t jpgLen = 0;
  uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
  if (!jpg) {
    sendTelegramMessageTo(chatId, cfg.name + ": snapshot fetch failed - see Serial log.");
    return;
  }

  String caption = cfg.name + " - " + nowTimestampString();
  if (!sendTelegramPhotoWithRetry(jpg, jpgLen, caption, chatId)) {
    Serial.printf("[%s] On-demand snapshot send to chat %s failed.\n", cfg.name.c_str(), chatId.c_str());
  }
  pushCameraSnapshot(cfg, st, jpg, jpgLen); // takes ownership - do not free(jpg) here
}

// Result of resolveAlertTimer below - shared by the single-camera and
// all-cameras /on//off paths in handleTelegramCommand/handleAllCamerasCommand
// so the duration-parsing logic (and its error handling) can't drift
// between the two copies the way independently-duplicated parsing already
// caused a real bug once in this project (the /reset reboot loop).
struct AlertTimer {
  bool ok = true;               // false only if durationText was non-empty and unparseable
  bool hasTimer = false;        // true if durationText was non-empty and DID parse
  unsigned long revertDueMs = 0; // valid only if hasTimer
  String suffix;                 // " (auto ON in 1h30m)" etc, "" if !hasTimer - appended to the reply
  String errorMsg;               // set only if !ok - what to reply with
};

// durationText is parsed.durationText ("" means no timer, permanent
// on/off - the original behavior). Resolving "HH:MM" needs the actual
// current local time, which parseDurationToken (telegram_parse.h)
// deliberately doesn't read for itself - see its own comment.
static AlertTimer resolveAlertTimer(const String& durationText, bool turnOn) {
  AlertTimer result;
  if (durationText.length() == 0) return result;

  time_t now; time(&now);
  struct tm nowLocal; localtime_r(&now, &nowLocal);
  ParsedDuration dur = parseDurationToken(durationText, nowLocal);
  if (!dur.ok) {
    result.ok = false;
    result.errorMsg = "Couldn't understand duration \"" + durationText +
                       "\" - use a number of minutes (e.g. \"30\") or a 24h clock time (e.g. \"23:00\").";
    return result;
  }
  result.hasTimer = true;
  result.revertDueMs = millis() + dur.secondsFromNow * 1000UL;
  result.suffix = " (auto " + String(turnOn ? "OFF" : "ON") + " in " + formatUptime(dur.secondsFromNow * 1000UL) + ")";
  return result;
}

// Applies /on all, /off all [duration], or /snap all to every currently-
// enabled camera - see pollTelegramCommands' (telegram.h) comment on the
// "all" keyword for the (extremely narrow) trade-off it makes against a
// real camera named starting with "all". Caller (handleTelegramCommand)
// has already matched parsed.cameraName == "all" case-insensitively
// before reaching here.
static void handleAllCamerasCommand(const TelegramUser& sender, const ParsedTelegramCommand& parsed,
                                     const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  std::vector<size_t> targets;
  for (size_t i = 0; i < numCameras; i++) {
    if (cameras[i].enabled) targets.push_back(i);
  }
  if (targets.empty()) {
    sendTelegramMessageTo(sender.chatId, "No enabled cameras to apply this to.");
    return;
  }

  if (parsed.command == TelegramCommand::Snap) {
    Serial.printf("[Telegram] On-demand snapshot of all %u camera(s) requested by user \"%s\".\n",
                  (unsigned)targets.size(), sender.name.c_str());
    for (size_t i : targets) {
      sendOnDemandSnapshot(cameras[i], states[i], sender.chatId);
      // A fetch+send per camera, synchronously, all within this one
      // loop() tick - main.cpp's loop() only resets the task watchdog at
      // its own top, so enough slow/unresponsive cameras in one "/snap
      // all" could otherwise add up toward WATCHDOG_TIMEOUT_MS (90s) and
      // panic-reboot the board over a Telegram command. Same reasoning,
      // same fix, as camera_store.cpp's restoreMissingCamerasFromSeed().
      esp_task_wdt_reset();
    }
    return;
  }

  bool turnOn = (parsed.command == TelegramCommand::On);
  AlertTimer timer = resolveAlertTimer(parsed.durationText, turnOn);
  if (!timer.ok) {
    Serial.printf("[Telegram] /%s all from user \"%s\" has an unparseable duration \"%s\".\n",
                  turnOn ? "on" : "off", sender.name.c_str(), parsed.durationText.c_str());
    sendTelegramMessageTo(sender.chatId, timer.errorMsg);
    return;
  }

  for (size_t i : targets) {
    { CameraStateLock lock(states[i]); states[i].alertsEnabled = turnOn;
      states[i].scheduledRevertDueMs = timer.hasTimer ? timer.revertDueMs : 0;
      states[i].scheduledRevertToOn = !turnOn; }
    saveAlertEnabledPref(i, turnOn);
  }
  Serial.printf("[Telegram] Alerts turned %s for all %u camera(s) via Telegram by user \"%s\"%s.\n",
                turnOn ? "ON" : "OFF", (unsigned)targets.size(), sender.name.c_str(), timer.hasTimer ? " (timed)" : "");
  logEvent("All cameras alerts: " + String(turnOn ? "ON" : "OFF") + " via " + sender.name + timer.suffix);
  sendTelegramMessageTo(sender.chatId, "All " + String(targets.size()) + " camera(s) alerts: " +
                                        (turnOn ? "ON" : "OFF") + timer.suffix);
}

// lastUpdateId is this poll's running highest update_id, already advanced
// past `text`'s own update - passed through so the Reset case below can
// persist it immediately, before ESP.restart(). See this section's top
// comment for why that decision belongs here and not in pollTelegramCommands.
//
// text is parsed exactly once, by parseTelegramCommand (telegram_parse.h) -
// command identity, required permission, and (for On/Off/Snap) the camera
// name are all decided there and used as-is below, instead of being
// re-derived a second time here the way an earlier version did. The
// switch below has no default case, and platformio.ini enables
// -Werror=switch specifically because this toolchain's default flags
// otherwise accept a non-exhaustive switch silently (verified, not
// assumed) - so a new TelegramCommand added without a case here is a
// build failure, not a silent "unrecognized, ignored".
static void handleTelegramCommand(const TelegramUser& sender, const String& text, const CameraConfig cameras[],
                                   CameraState states[], size_t numCameras, long lastUpdateId) {
  Serial.printf("[Telegram] Command from user \"%s\": \"%s\"\n", sender.name.c_str(), text.c_str());

  ParsedTelegramCommand parsed = parseTelegramCommand(text);

  bool authorized = false;
  switch (parsed.requiredPermission) {
    case TelegramCommandPermission::Command: authorized = sender.canCommand; break;
    case TelegramCommandPermission::Snap:    authorized = sender.canSnap;    break;
    case TelegramCommandPermission::Reset:   authorized = sender.canReset;   break;
    case TelegramCommandPermission::Unknown: authorized = false;             break;
  }
  // Logged here for every command now, including /status/uptime/reset -
  // an earlier version of this check only logged rejected /on//off/snap
  // attempts, silently replying with no server-side trace for the others.
  // Calling that out explicitly since it wasn't when this unification
  // first landed.
  if (parsed.requiredPermission != TelegramCommandPermission::Unknown && !authorized) {
    String name = commandDisplayName(parsed.command);
    Serial.printf("[Telegram] User \"%s\" not authorized for %s.\n", sender.name.c_str(), name.c_str());
    sendTelegramMessageTo(sender.chatId, "You're not authorized to use " + name + ".");
    return;
  }

  switch (parsed.command) {
    case TelegramCommand::Status: {
      // isOffline and the timer fields are written by other tasks
      // (camera.cpp's own task, and loop()'s task via handleTelegramCommand
      // /checkScheduledAlertReverts - this read runs on loop()'s task too,
      // but isOffline specifically crosses from the camera's own task, so
      // the whole group is read under one lock for simplicity rather than
      // splitting into a locked and an unlocked half. See
      // CameraState::stateMutex.
      String msg = "Camera alert status:\n";
      for (size_t i = 0; i < numCameras; i++) {
        if (!cameras[i].enabled) continue;
        bool alertsEnabled, offline;
        unsigned long revertDueMs;
        bool revertToOn;
        {
          CameraStateLock lock(states[i]);
          alertsEnabled = states[i].alertsEnabled;
          offline = states[i].isOffline;
          revertDueMs = states[i].scheduledRevertDueMs;
          revertToOn = states[i].scheduledRevertToOn;
        }
        msg += String(cameras[i].name) + ": " + (alertsEnabled ? "ON" : "OFF");
        if (offline) msg += " - OFFLINE";
        if (revertDueMs != 0 && (long)(millis() - revertDueMs) < 0) {
          msg += " (auto " + String(revertToOn ? "ON" : "OFF") + " in " +
                 formatUptime(revertDueMs - millis()) + ")";
        }
        msg += "\n";
      }
      Serial.printf("[Telegram] Replying to user \"%s\" with camera status.\n", sender.name.c_str());
      sendTelegramMessageTo(sender.chatId, msg);
      return;
    }

    case TelegramCommand::Uptime:
      Serial.printf("[Telegram] Replying to user \"%s\" with uptime.\n", sender.name.c_str());
      sendTelegramMessageTo(sender.chatId, "Uptime: " + formatUptime(millis()));
      return;

    case TelegramCommand::Reset:
      Serial.printf("[Telegram] Reboot requested by user \"%s\" via /reset.\n", sender.name.c_str());
      // Persisted here, immediately before the irreversible action - see
      // this function's own top comment for why it's decided here and not
      // by pollTelegramCommands ahead of time.
      saveLastUpdateId(lastUpdateId);
      // Reply before restarting - ESP.restart() never returns, so this is
      // the last chance to confirm the command was actually received.
      sendTelegramMessageTo(sender.chatId, "\xE2\x99\xBB\xEF\xB8\x8F Rebooting now...");
      delay(500); // let the TLS send above finish flushing before the reboot tears down WiFi
      // ESP.restart() doesn't wait for other FreeRTOS tasks to finish
      // whatever they're doing - if a camera task is mid-write to SD at
      // this exact moment, an uncoordinated reset could corrupt more than
      // just that one file (FAT isn't a journaling filesystem). See
      // waitForSdIdle's own comment (sd_store.h) for the full reasoning;
      // no-op if SD isn't active.
      waitForSdIdle();
      ESP.restart();
      return; // unreachable - ESP.restart() doesn't return - kept for a tidy switch

    case TelegramCommand::On:
    case TelegramCommand::Off:
    case TelegramCommand::Snap:
      break; // handled below - shares the camera-name-matching logic

    case TelegramCommand::Help: {
      Serial.printf("[Telegram] Replying to user \"%s\" with /help.\n", sender.name.c_str());
      String msg =
          "/status - list every camera's alert status\n"
          "/uptime - board uptime\n"
          "/on <camera|all> [duration] - resume alerts\n"
          "/off <camera|all> [duration] - mute alerts\n"
          "/snap <camera|all> - fresh photo now, ignoring mute/cooldown\n"
          "/health - board health (heap, PSRAM, NVS, WiFi signal, SD storage)\n"
          "/reset - reboot the board immediately\n"
          "/help - this message\n\n"
          "<camera> matches by name or prefix; \"all\" applies to every enabled camera.\n"
          "[duration] is optional: a number of minutes, or a 24h clock time like "
          "\"23:00\" (next occurrence - tomorrow if that time already passed today). "
          "Omitted means permanent.\n\n"
          "Your permissions: canCommand=" + String(sender.canCommand ? "yes" : "no") +
          ", canSnap=" + String(sender.canSnap ? "yes" : "no") +
          ", canReset=" + String(sender.canReset ? "yes" : "no");
      sendTelegramMessageTo(sender.chatId, msg);
      return;
    }

    case TelegramCommand::Health: {
      Serial.printf("[Telegram] Replying to user \"%s\" with /health.\n", sender.name.c_str());

      nvs_stats_t nvsStats;
      bool haveNvsStats = (nvs_get_stats(NULL, &nvsStats) == ESP_OK) && nvsStats.total_entries > 0;

      SdStatus sd = getSdStatus();
      String sdLine;
      if (!sd.settingEnabled) {
        sdLine = "disabled (PSRAM-only snapshot history)";
      } else if (!sd.available) {
        sdLine = "enabled but not detected (PSRAM-only fallback active)";
      } else {
        sdLine = sd.cardTypeName + ", " + String((double)sd.usedBytes / (1024.0 * 1024.0), 1) +
                 " MB / " + String((double)sd.totalBytes / (1024.0 * 1024.0), 1) + " MB used";
      }

      String msg = "Board health:\n";
      msg += "Uptime: " + formatUptime(millis()) + "\n";
      msg += "Free heap: " + String(ESP.getFreeHeap()) + " bytes (min ever: " +
             String(ESP.getMinFreeHeap()) + ")\n";
      msg += "Free PSRAM: " + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + " bytes\n";
      if (haveNvsStats) {
        unsigned pct = (unsigned)((uint64_t)nvsStats.used_entries * 100 / nvsStats.total_entries);
        msg += "NVS usage: " + String(pct) + "% (" + String((unsigned)nvsStats.used_entries) + " / " +
               String((unsigned)nvsStats.total_entries) + " entries)\n";
      }
      msg += "WiFi signal: " + String(WiFi.RSSI()) + " dBm\n";
      msg += "SD storage: " + sdLine;

      sendTelegramMessageTo(sender.chatId, msg);
      return;
    }

    case TelegramCommand::Unknown:
      Serial.println("[Telegram] Unrecognized command - ignored.");
      return;
  }

  // "all" (case-insensitive) is a special target meaning every enabled
  // camera at once, matched here rather than by matchCamerasByPrefix below
  // - see pollTelegramCommands' (telegram.h) comment on the trade-off that
  // makes for a real camera named starting with "all".
  String cameraNameLower = parsed.cameraName;
  cameraNameLower.toLowerCase();
  if (cameraNameLower == "all") {
    handleAllCamerasCommand(sender, parsed, cameras, states, numCameras);
    return;
  }

  // Matched by prefix ("D01" matches "D01-FDir") - ambiguous matches get
  // nothing applied and a reply listing what matched, rather than guessing.
  std::vector<size_t> matches = matchCamerasByPrefix(cameras, numCameras, parsed.cameraName);
  String verb = commandDisplayName(parsed.command).substring(1); // drop the leading "/" for mid-sentence use

  if (matches.size() > 1) {
    String list;
    for (size_t idx : matches) { if (list.length() > 0) list += ", "; list += cameras[idx].name; }
    Serial.printf("[Telegram] /%s target \"%s\" from user \"%s\" is ambiguous: %s\n", verb.c_str(),
                  parsed.cameraName.c_str(), sender.name.c_str(), list.c_str());
    sendTelegramMessageTo(sender.chatId, "\"" + parsed.cameraName + "\" matches more than one camera: " + list +
                                          " - be more specific.");
    return;
  }

  if (matches.empty()) {
    Serial.printf("[Telegram] /%s target not found or disabled: \"%s\" (user \"%s\")\n", verb.c_str(),
                  parsed.cameraName.c_str(), sender.name.c_str());
    sendTelegramMessageTo(sender.chatId, "Unknown or disabled camera: " + parsed.cameraName);
    return;
  }

  size_t i = matches[0];

  if (parsed.command == TelegramCommand::Snap) {
    Serial.printf("[%s] On-demand snapshot requested by user \"%s\" via Telegram.\n",
                  cameras[i].name.c_str(), sender.name.c_str());
    sendOnDemandSnapshot(cameras[i], states[i], sender.chatId);
    return;
  }

  bool turnOn = (parsed.command == TelegramCommand::On);

  // parsed.durationText is "" for a plain /on or /off (permanent, the
  // original behavior) - only non-empty when a timer token followed the
  // camera name (see parseTelegramCommand's comment). See resolveAlertTimer's
  // own comment for why the actual parsing lives there, shared with the
  // "/on all"/"/off all" path (handleAllCamerasCommand) above.
  AlertTimer timer = resolveAlertTimer(parsed.durationText, turnOn);
  if (!timer.ok) {
    Serial.printf("[Telegram] /%s target \"%s\" from user \"%s\" has an unparseable duration \"%s\".\n",
                  verb.c_str(), cameras[i].name.c_str(), sender.name.c_str(), parsed.durationText.c_str());
    sendTelegramMessageTo(sender.chatId, timer.errorMsg);
    return;
  }

  {
    CameraStateLock lock(states[i]); // read cross-task by camera.cpp/webserver.cpp
    states[i].alertsEnabled = turnOn;
    // A plain (no-timer) /on or /off cancels whatever timer was pending
    // before - issuing a new command always replaces the old schedule,
    // never stacks with it.
    states[i].scheduledRevertDueMs = timer.hasTimer ? timer.revertDueMs : 0;
    states[i].scheduledRevertToOn = !turnOn;
  }
  saveAlertEnabledPref(i, turnOn);
  Serial.printf("[%s] Alerts turned %s via Telegram by user \"%s\"%s.\n", cameras[i].name.c_str(),
                turnOn ? "ON" : "OFF", sender.name.c_str(), timer.hasTimer ? " (timed)" : "");
  logEvent(String(cameras[i].name) + " alerts: " + (turnOn ? "ON" : "OFF") + " via " + sender.name + timer.suffix);
  sendTelegramMessageTo(sender.chatId,
                         String(cameras[i].name) + " alerts: " + (turnOn ? "ON" : "OFF") + timer.suffix);
}

// Called once per loop() tick (main.cpp). Cheap: just a millis() comparison
// per camera when nothing's due. Overflow-safe comparison (see
// CameraState::scheduledRevertDueMs's comment) matches main.cpp's own
// g_wifiRetryDueMs pattern.
void checkScheduledAlertReverts(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  for (size_t i = 0; i < numCameras; i++) {
    unsigned long dueMs;
    bool revertToOn;
    {
      CameraStateLock lock(states[i]);
      dueMs = states[i].scheduledRevertDueMs;
      revertToOn = states[i].scheduledRevertToOn;
    }
    if (dueMs == 0) continue; // no timer pending for this camera
    if ((long)(millis() - dueMs) < 0) continue; // not due yet

    { CameraStateLock lock(states[i]); states[i].alertsEnabled = revertToOn; states[i].scheduledRevertDueMs = 0; }
    saveAlertEnabledPref(i, revertToOn);
    Serial.printf("[%s] Timed alert window expired - alerts turned %s automatically.\n",
                  cameras[i].name.c_str(), revertToOn ? "ON" : "OFF");
    logEvent(String(cameras[i].name) + " alerts: " + (revertToOn ? "ON" : "OFF") + " (timer expired)");
    sendTelegramMessage(String(cameras[i].name) + " alerts: " + (revertToOn ? "ON" : "OFF") + " (timer expired)");
  }
}

void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  // -1 sentinel: load the real value from NVS on this function's first
  // call only, rather than starting at 0 every boot - see this section's
  // top comment for why a reboot redelivering old updates is dangerous
  // now that /reset exists. Real Telegram update_ids are always >= 0.
  static long lastUpdateId = -1;
  if (lastUpdateId < 0) lastUpdateId = loadLastUpdateId();
  long startingUpdateId = lastUpdateId; // so the end-of-poll persist below is skipped when nothing advanced

  // HTTPClient rather than a raw socket + hand-rolled "\r\n\r\n" body split
  // (see sendTelegramMessageTo's comment) - that split silently mis-parses
  // if Telegram ever sends a chunked response, since it doesn't decode
  // chunk-size markers before handing the body to parseTelegramUpdates.
  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA);

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) +
               "/getUpdates?offset=" + String(lastUpdateId + 1) + "&timeout=0";
  if (!http.begin(client, url)) {
    Serial.println("[Telegram] pollTelegramCommands: http.begin() failed.");
    return;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);

  int code = http.GET();
  if (code != 200) {
    String detail = (code > 0) ? String("") : (" - " + HTTPClient::errorToString(code));
    Serial.printf("[Telegram] pollTelegramCommands: HTTP %d%s\n", code, detail.c_str());
    http.end();
    return; // transient - next poll retries
  }
  String body = http.getString();
  http.end();

  String parseError;
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body, &parseError);
  if (parseError.length() > 0) {
    Serial.printf("[Telegram] pollTelegramCommands: %s\n", parseError.c_str());
  }

  std::vector<TelegramUser> users = loadTelegramUsers();
  for (auto& upd : updates) {
    // Advanced in RAM for every update (keeps the offset moving so the
    // same batch isn't refetched next poll), but NOT persisted to NVS here
    // - Telegram delivers every inbound message to getUpdates regardless
    // of sender, so persisting per-update would let anyone who finds this
    // bot force an NVS write just by sending it messages, no permission
    // required. Persisted once at the very end of this loop instead,
    // covering the normal case (redelivering an already-applied /on/off/
    // snap/status/uptime after a reboot is harmless - see this section's
    // top comment) in one write per poll regardless of how many messages
    // arrived, spam included.
    if (upd.updateId > lastUpdateId) lastUpdateId = upd.updateId;
    if (!upd.hasChatId) continue; // no message on this update (edited_message, channel_post, ...)

    const TelegramUser* sender = nullptr;
    for (auto& u : users) {
      if (chatIdMatches(u.chatId, upd.chatId)) { sender = &u; break; }
    }
    // canCommand, canSnap, and canReset are independent permissions (see
    // TelegramUser) - a sender needs at least one of them to reach
    // handleTelegramCommand at all; which specific commands that actually
    // unlocks is decided there, per-command.
    if (!sender || !(sender->canCommand || sender->canSnap || sender->canReset)) {
      if (sender) {
        Serial.printf("[Telegram] Ignored command from %s (not authorized to send commands)\n",
                      sender->name.c_str());
      } else {
        Serial.printf("[Telegram] Ignored command from unknown chat ID %lld\n", (long long)upd.chatId);
      }
      continue;
    }

    if (upd.text.length() == 0) continue; // e.g. a sticker or photo with no caption - nothing to act on

    // lastUpdateId passed through so handleTelegramCommand's /reset branch
    // can persist it immediately, before doing anything irreversible - see
    // that function's own top comment for why this isn't decided here.
    handleTelegramCommand(*sender, upd.text, cameras, states, numCameras, lastUpdateId);
  }
  if (lastUpdateId != startingUpdateId) saveLastUpdateId(lastUpdateId);
}
