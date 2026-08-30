#include "telegram.h"
#include "telegram_ca.h"
#include "telegram_users.h"
#include "telegram_parse.h"
#include "telegram_multipart.h"
#include "format_utils.h"
#include "event_log_store.h"
#include "snapshot_history.h"
#include "sd_store.h"
#include "quiet_hours.h"
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
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include <cstring>
#include <vector>
#include <algorithm>

bool telegramCAConfigured() {
  // Sanity check that this actually looks like a PEM certificate, not a
  // placeholder/filename/empty string left behind by mistake.
  return strstr(TELEGRAM_ROOT_CA, "-----BEGIN CERTIFICATE-----") != nullptr &&
         strstr(TELEGRAM_ROOT_CA, "-----END CERTIFICATE-----") != nullptr;
}

// Serializes every outbound TLS session this file opens to api.telegram.org
// (photo sends, JSON API calls, getUpdates polling) across every task that
// calls into this file. Each camera runs its own independent FreeRTOS task
// (camera_tasks.h's cameraTaskFn) with nothing otherwise stopping two or
// more from being mid-send at once; WiFiClientSecure's mbedTLS session
// state is allocated from internal RAM, not PSRAM, and platformio.ini does
// no MBEDTLS buffer tuning. A real multi-camera motion burst has been
// observed in the field driving free heap down to ~70KB and failing
// outright with "writeAllBytes: stalled, no progress for 5s" followed by
// "SSL - Memory allocation failed" - this bounds concurrent TLS sessions to
// Telegram to exactly one at a time, project-wide. Deliberately does NOT
// cover fetchOneSnapshot's HTTP GET to the camera itself below - that's a
// different, uncontended network resource (plain HTTP to a LAN device),
// not part of this budget.
static SemaphoreHandle_t g_telegramNetMutex = xSemaphoreCreateMutex();

// RAII wrapper for g_telegramNetMutex - bounded xSemaphoreTake
// (TELEGRAM_NET_MUTEX_TIMEOUT_MS, config.h) with guaranteed release on
// every return path. Unlike CameraStateLock (camera.h), the wait here is
// intentionally bounded, not portMAX_DELAY: a camera task stuck waiting
// forever for Telegram send capacity would also stop servicing its own
// ONVIF PullMessages/subscription-renewal loop. A held()==false timeout is
// treated exactly like any other failed send by every caller below -
// logged, non-fatal, never an indefinite block.
class TelegramNetLock {
 public:
  TelegramNetLock()
      : held_(xSemaphoreTake(g_telegramNetMutex, pdMS_TO_TICKS(TELEGRAM_NET_MUTEX_TIMEOUT_MS)) == pdTRUE) {}
  ~TelegramNetLock() { if (held_) xSemaphoreGive(g_telegramNetMutex); }
  bool held() const { return held_; }
  TelegramNetLock(const TelegramNetLock&) = delete;
  TelegramNetLock& operator=(const TelegramNetLock&) = delete;

 private:
  bool held_;
};

// Camera -> Telegram. PSRAM is a hard requirement (main.cpp's setup()
// refuses to boot without it): a motion alert can go to more than one
// user, so the JPEG is fetched once and resent per recipient (see
// triggerMotionAlert below). SNAPSHOT_MAX_BYTES only matters as
// allocateSnapshotBuffer's fallback cap if a PSRAM allocation fails.

// Allocates up to `cap` bytes, preferring PSRAM. On failure, falls back to
// internal RAM but capped at SNAPSHOT_MAX_BYTES, not the original
// (possibly much larger) `cap` - internal RAM is scarce here, so retrying
// the exact size that just failed on PSRAM would likely just fail again.
// `cap` is updated in place to whatever was actually allocated.
static uint8_t* allocateSnapshotBuffer(size_t& cap) {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (buf) return buf;

  if (cap > SNAPSHOT_MAX_BYTES) cap = SNAPSHOT_MAX_BYTES;
  return (uint8_t*)malloc(cap);
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

// Buffers a snapshot fully (PSRAM if available, else internal RAM) up to
// `cap` bytes. Caller must free() the returned buffer.
//
// Reads via http.getStreamPtr() - the raw socket, bypassing HTTPClient's
// own response-body decoding. Fine for a Content-Length-known or
// connection-close-delimited body, but must never be used for a
// Transfer-Encoding: chunked response, where these "bytes" would actually
// be raw chunk-size/CRLF framing interleaved with the real data,
// corrupting the JPEG. See fetchSnapshotBufferedChunked below for that
// case - fetchOneSnapshot picks the right one.
static uint8_t* fetchSnapshotBuffered(HTTPClient& http, size_t& outLen, size_t cap) {
  outLen = 0;
  uint8_t* buf = allocateSnapshotBuffer(cap); // cap may shrink here (internal-RAM fallback) - read that back below
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

// Buffers a chunked-transfer-encoded snapshot via HTTPClient::getString(),
// which correctly decodes chunk framing (unlike fetchSnapshotBuffered's
// raw stream reads). Only used when the response actually IS chunked.
// getString() reads the whole body into one String regardless of size, so
// it's capped here rather than bounded during the read - acceptable for a
// camera this project's user configured and trusts, not adversarial
// input. Caller must free() the returned buffer.
static uint8_t* fetchSnapshotBufferedChunked(HTTPClient& http, size_t& outLen) {
  outLen = 0;
  String body = http.getString();
  size_t len = body.length();
  if (len == 0) return nullptr;
  if (len > SNAPSHOT_MAX_BYTES_PSRAM) {
    Serial.printf("Chunked snapshot body too large (%u bytes) - discarding.\n", (unsigned)len);
    return nullptr;
  }
  uint8_t* buf = allocateSnapshotBuffer(len);
  if (!buf) {
    Serial.println("Snapshot buffer allocation failed (chunked path).");
    return nullptr;
  }
  memcpy(buf, body.c_str(), len); // length-bounded, not strlen-based - safe for binary content
  outLen = len;
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

  // See g_telegramNetMutex's own comment - a real incident, not
  // theoretical: a multi-camera motion burst has driven free heap to
  // ~70KB and failed outright with SSL alloc errors when more than one
  // camera's TLS session to Telegram was open at once.
  TelegramNetLock netLock;
  if (!netLock.held()) {
    Serial.println("Telegram sendPhoto: timed out waiting for Telegram send capacity - skipping.");
    return false;
  }

  Serial.printf("Free heap before Telegram send: %u bytes (max alloc: %u, jpg: %u bytes)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)jpgLen);

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000); // seconds, not ms - unlike every other timeout in this file
  if (!client.connect("api.telegram.org", 443, HTTP_TIMEOUT_MS)) {
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

// Same "has NTP actually set the clock at least once" check
// parseDurationToken (telegram_parse.h) already uses for its own HH:MM
// resolution - quiet hours must fail OPEN (alerts still send normally)
// against an unsynced, near-epoch clock, not silently misjudge the
// window on a security-relevant feature.
static bool localClockSynced() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  return tmStruct.tm_year > (2016 - 1900);
}

static int currentLocalMinuteOfDay() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  return tmStruct.tm_hour * 60 + tmStruct.tm_min;
}

// Shared outbound JSON-POST mechanics for every Telegram Bot API method
// this project calls with a JSON body - sendMessage (plain or with an
// inline keyboard) and answerCallbackQuery. `method` is the API method
// name; `doc` is the caller's already-built request body.
//
// Uses HTTPClient, not a raw WiFiClientSecure + hand-built request line
// (unlike sendTelegramPhotoBuffered, which streams a multipart body
// HTTPClient can't) - HTTPClient correctly handles chunked
// transfer-encoding on the response, which api.telegram.org has been
// observed to send and a manual parser wouldn't.
static bool sendTelegramApiCall(const String& method, JsonDocument& doc) {
  // See g_telegramNetMutex's own comment.
  TelegramNetLock netLock;
  if (!netLock.held()) {
    Serial.printf("sendTelegramApiCall(%s): timed out waiting for Telegram send capacity - skipping.\n",
                  method.c_str());
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000); // seconds, not ms - see sendTelegramPhotoBuffered's comment

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) + "/" + method;
  if (!http.begin(client, url)) {
    Serial.printf("sendTelegramApiCall(%s): http.begin() failed.\n", method.c_str());
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  bool ok = (code == 200);
  if (!ok) {
    Serial.printf("sendTelegramApiCall(%s): HTTP %d", method.c_str(), code);
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

static bool sendTelegramMessageTo(const String& chatId, const String& text) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = text;
  return sendTelegramApiCall("sendMessage", doc);
}

// Sends text with an inline keyboard, one button per row - `buttons` is
// (label, callback_data) pairs. handleTelegramCallbackQuery (below)
// receives a tapped button's callback_data back on the next poll. Skips
// (and logs) any button whose callback_data would exceed Telegram's
// 64-byte limit rather than sending a broken button - not expected to
// trigger with this project's camera names, but if `outSkipped` is
// non-null it's set to how many were dropped, so the caller can still
// tell the user instead of the gap being silent past the Serial log.
static bool sendTelegramKeyboardTo(const String& chatId, const String& text,
                                    const std::vector<std::pair<String, String>>& buttons,
                                    size_t* outSkipped = nullptr) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = text;
  JsonArray rows = doc["reply_markup"]["inline_keyboard"].to<JsonArray>();
  size_t skipped = 0;
  for (auto& b : buttons) {
    if (b.second.length() > 64) {
      Serial.printf("[Telegram] Skipping button \"%s\" - callback_data too long (%u bytes).\n",
                    b.first.c_str(), (unsigned)b.second.length());
      skipped++;
      continue;
    }
    JsonArray row = rows.add<JsonArray>();
    JsonObject btn = row.add<JsonObject>();
    btn["text"] = b.first;
    btn["callback_data"] = b.second;
  }
  if (outSkipped) *outSkipped = skipped;
  return sendTelegramApiCall("sendMessage", doc);
}

// Acknowledges a button tap - clears its client-side loading spinner.
// `text` (optional, "" for none) shows as a brief toast, not a chat
// message - handleTelegramCallbackQuery still sends a real confirmation
// message separately for anything worth keeping in the chat history.
static bool answerTelegramCallback(const String& callbackQueryId, const String& text) {
  JsonDocument doc;
  doc["callback_query_id"] = callbackQueryId;
  if (text.length() > 0) doc["text"] = text;
  return sendTelegramApiCall("answerCallbackQuery", doc);
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
  // Registered so http.header() can actually see it after GET() - without
  // collectHeaders(), HTTPClient parses Transfer-Encoding internally for
  // its own use but doesn't expose it through header() at all. See
  // fetchSnapshotBuffered's own comment for why detecting this matters.
  static const char* kCollectedHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(kCollectedHeaders, 1);

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

  String transferEncoding = http.header("Transfer-Encoding");
  transferEncoding.toLowerCase();

  size_t jpgLen = 0;
  uint8_t* jpg = (transferEncoding.indexOf("chunked") >= 0) ? fetchSnapshotBufferedChunked(http, jpgLen)
                                                              : fetchSnapshotBuffered(http, jpgLen, cap);
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
  if (st.hasAlerted && nowMs - st.lastAlert < cfg.alertCooldownMs) {
    // Only counted while a real snapshot send (below) is what started this
    // cooldown - digestArmed stays false through a quiet-hours cycle (see
    // that branch below), so motion suppressed during a quiet stretch
    // doesn't get reported as "since the snapshot" when no snapshot was
    // actually sent to anyone. See checkPendingMotionDigest for the flush.
    if (st.digestArmed) st.suppressedMotionCount++;
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
    { CameraStateLock lock(st); st.lastAlert = nowMs; st.hasAlerted = true; }
    logEvent(cfg.name + ": motion detected (quiet hours - no Telegram alert)");
    size_t jpgLen = 0;
    uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
    if (jpg) pushCameraSnapshot(cfg, st, jpg, jpgLen); // takes ownership - do not free(jpg) here
    return;
  }

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
  // Starts a fresh digest cycle for checkPendingMotionDigest - see
  // suppressedMotionCount's own comment (camera.h). Reset here, not just
  // left to accumulate, so a digest never double-counts events already
  // reported by a previous cycle's flush.
  st.digestArmed = true;
  st.suppressedMotionCount = 0;

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

void triggerTimelapseCapture(const CameraConfig& cfg, CameraState& st) {
  // snapshotUri is written only by this camera's own task (camera.cpp),
  // same-task self-read, no lock needed - same reasoning as
  // triggerMotionAlert's own check.
  if (st.snapshotUri.length() == 0) return;

  size_t jpgLen = 0;
  uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
  if (!jpg) return; // logged by fetchOneSnapshot itself
  pushCameraSnapshot(cfg, st, jpg, jpgLen); // takes ownership - do not free(jpg) here
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
  // lastContactMs is lock-guarded (see cameraSoapCall's/pushCameraSnapshot's
  // own comments) - pushCameraSnapshot can adjust it from loop()'s task
  // (sendOnDemandSnapshot, via /snap), not just this camera's own task.
  unsigned long lastContactMs;
  { CameraStateLock lock(st); lastContactMs = st.lastContactMs; }
  bool offlineNow = (millis() - lastContactMs) >= cfg.offlineThresholdMs;
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
  sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F " + cfg.name + ": no motion detected in over " +
                       String((unsigned)cfg.motionWatchdogHours) + " hour(s) - check the camera/PIR.");
}

// Flushes triggerMotionAlert's suppressedMotionCount as one summary text
// once the cooldown it accumulated during ends - so "5 more motion events
// since the snapshot" (motion kept happening) and silence (it was a
// one-off) are both visible, instead of every event after the first
// simply vanishing until the next real send. digestArmed/
// suppressedMotionCount are same-task-only (see their own comments,
// camera.h), same reasoning as checkMotionWatchdog above - no lock needed.
void checkPendingMotionDigest(const CameraConfig& cfg, CameraState& st) {
  if (!st.digestArmed) return; // no real send is currently being tracked
  if (millis() - st.lastAlert < cfg.alertCooldownMs) return; // cooldown still running - not due yet

  st.digestArmed = false; // one flush per cooldown cycle, whatever the count
  uint32_t count = st.suppressedMotionCount;
  st.suppressedMotionCount = 0;
  if (count == 0) return; // genuinely a one-off - nothing to report

  // alertsEnabled is written cross-task (pollTelegramCommands' /on//off) -
  // needs the lock, same as triggerMotionAlert's own read of it.
  bool alertsEnabled;
  { CameraStateLock lock(st); alertsEnabled = st.alertsEnabled; }
  if (!alertsEnabled) return; // muted since the snapshot went out - stay quiet

  std::vector<String> recipients;
  for (auto& u : loadTelegramUsers()) {
    if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back(u.chatId);
  }
  if (recipients.empty()) return;

  String msg = cfg.name + ": motion continued - " + String(count) +
               " more event(s) since the last snapshot.";
  for (auto& chatId : recipients) sendTelegramMessageTo(chatId, msg);
  logEvent(cfg.name + ": motion digest - " + String(count) + " event(s) since last snapshot");
}

// ============================================================
// Remote on/off control (Telegram commands)
//
// pollTelegramCommands() runs periodically from loop() (short getUpdates,
// not long-poll). lastUpdateId is persisted in NVS - it used not to be, on
// the theory that redelivering a couple of already-applied idempotent
// commands after a reboot is harmless. /reset broke that: redelivering it
// after the reboot it caused re-executes /reset again, forever - a real
// infinite reboot loop hit the first time /reset was used.
//
// Persisting on every update closed that loop but opened a smaller one:
// Telegram delivers every inbound message regardless of sender, so an
// unauthenticated flood would force an NVS write per message. Persisting
// once per poll instead (below) bounds that while keeping the original
// redelivery assumption for everything except /reset.
//
// /reset can't wait for that end-of-poll persist - ESP.restart() never
// returns - so it's persisted inside handleTelegramCommand's own /reset
// branch, immediately before the restart, rather than pollTelegramCommands
// pre-guessing which commands are "dangerous" (an earlier version did
// exactly that, checking canReset/the command text in two places that
// drifted out of sync with each other).
// ============================================================

static const char* TELEGRAM_STATE_NAMESPACE = "tgstate";
static const char* TELEGRAM_STATE_KEY_LAST_UPDATE_ID = "lastUpdateId";

static long loadLastUpdateId() {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(TELEGRAM_STATE_NAMESPACE, false);
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

// ============================================================
// Recent unrecognized chat IDs - RAM-only, small fixed table (not NVS/a
// growable log): purely a convenience so adding a new Telegram user can be
// copy-paste from the Users page instead of a side trip to @userinfobot or
// the raw getUpdates URL, for whoever most recently actually messaged this
// bot. Not a security log - deliberately doesn't grow, persist, or record
// anything about WHO/WHAT was sent, just "this chat ID messaged the bot
// recently" for the one specific case (!sender in pollTelegramCommands)
// where the sender isn't a configured user at all.
// ============================================================

// Internal-only add-on to the header's own UnknownChatSighting - `used`
// marks a still-empty slot, never exposed outside this file.
struct UnknownChatSlot {
  int64_t chatId = 0;
  unsigned long lastSeenMs = 0;
  bool used = false;
};
static UnknownChatSlot g_unknownChats[UNKNOWN_CHAT_TRACK_MAX];
static SemaphoreHandle_t g_unknownChatsMutex = xSemaphoreCreateMutex();

// Same exact-match-or-least-recently-seen-eviction shape as webserver.cpp's
// RateLimitMiddleware::findOrCreate - unrelated tables, same small-fixed-
// size-tracking problem.
static void recordUnknownChat(int64_t chatId) {
  xSemaphoreTake(g_unknownChatsMutex, portMAX_DELAY);
  UnknownChatSlot* slot = nullptr;
  for (auto& e : g_unknownChats) {
    if (e.used && e.chatId == chatId) { slot = &e; break; }
  }
  if (!slot) {
    slot = &g_unknownChats[0];
    for (auto& e : g_unknownChats) {
      if (!e.used) { slot = &e; break; }
      if (e.lastSeenMs < slot->lastSeenMs) slot = &e;
    }
  }
  slot->chatId = chatId;
  slot->lastSeenMs = millis();
  slot->used = true;
  xSemaphoreGive(g_unknownChatsMutex);
}

std::vector<UnknownChatSighting> recentUnknownChats() {
  std::vector<UnknownChatSighting> result;
  xSemaphoreTake(g_unknownChatsMutex, portMAX_DELAY);
  for (auto& e : g_unknownChats) {
    if (e.used) result.push_back({e.chatId, e.lastSeenMs});
  }
  xSemaphoreGive(g_unknownChatsMutex);
  std::sort(result.begin(), result.end(),
            [](const UnknownChatSighting& a, const UnknownChatSighting& b) { return a.lastSeenMs > b.lastSeenMs; });
  return result;
}

static const char* ALERT_PREF_NAMESPACE = "camctl";

bool loadAlertEnabledPref(size_t index) {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(ALERT_PREF_NAMESPACE, false);
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
                       "\" - use a number of minutes (e.g. \"30\", max " + String(MAX_DURATION_MINUTES) +
                       ") or a 24h clock time (e.g. \"23:00\").";
    return result;
  }
  result.hasTimer = true;
  result.revertDueMs = millis() + dur.secondsFromNow * 1000UL;
  result.suffix = " (auto " + String(turnOn ? "OFF" : "ON") + " in " + formatUptime(dur.secondsFromNow * 1000UL) + ")";
  return result;
}

// Sets one camera's alerts on/off, persists it (NVS), logs it, and
// replies with confirmation - the single-camera state-mutation tail
// shared by the text-command path (/on|/off <camera> [duration], see
// handleTelegramCommand's own tail below) and the inline-keyboard button
// path (handleTelegramCallbackQuery, always a default-constructed
// AlertTimer - permanent, no duration support via buttons). Sharing this
// one implementation is what keeps the button path from silently
// diverging from the text-command path on reboot-persistence (a change
// here or a forgotten saveAlertEnabledPref call would otherwise only be
// caught in one of the two places).
static void applyOnOffToCamera(const CameraConfig& cfg, CameraState& st, size_t index, bool turnOn,
                                const AlertTimer& timer, const String& viaWho, const String& replyChatId) {
  {
    CameraStateLock lock(st); // read cross-task by camera.cpp/webserver.cpp
    st.alertsEnabled = turnOn;
    // A plain (no-timer) /on or /off cancels whatever timer was pending
    // before - issuing a new command always replaces the old schedule,
    // never stacks with it.
    st.scheduledRevertDueMs = timer.hasTimer ? timer.revertDueMs : 0;
    st.scheduledRevertToOn = !turnOn;
  }
  saveAlertEnabledPref(index, turnOn);
  Serial.printf("[%s] Alerts turned %s via Telegram by user \"%s\"%s.\n", cfg.name.c_str(),
                turnOn ? "ON" : "OFF", viaWho.c_str(), timer.hasTimer ? " (timed)" : "");
  logEvent(cfg.name + " alerts: " + (turnOn ? "ON" : "OFF") + " via " + viaWho + timer.suffix);
  sendTelegramMessageTo(replyChatId, cfg.name + " alerts: " + (turnOn ? "ON" : "OFF") + timer.suffix);
}

// Shared by /on all, /off all [duration] (via handleAllCamerasCommand
// below) and the Cameras page's own "Mute all"/"Unmute all" buttons
// (webserver.cpp) - the actual state-mutation logic can't drift between
// the two front-ends the way applyOnOffToCamera already prevents for the
// single-camera case. durationText is parsed the same way /on's own timer
// token is ("" = permanent, a plain number of minutes, or "HH:MM" - see
// parseDurationToken, telegram_parse.h); viaWho is a short human label for
// the Serial/Activity log ("Telegram (name)", "the dashboard"). Returns a
// plain-text result - success or the specific reason nothing happened
// (no enabled cameras, or an unparseable duration) - for the caller to
// relay however it likes (a Telegram reply, a web banner).
String setAllCamerasAlertState(const CameraConfig cameras[], CameraState states[], size_t numCameras,
                                bool turnOn, const String& durationText, const String& viaWho) {
  std::vector<size_t> targets;
  for (size_t i = 0; i < numCameras; i++) {
    if (cameras[i].enabled) targets.push_back(i);
  }
  if (targets.empty()) return "No enabled cameras to apply this to.";

  AlertTimer timer = resolveAlertTimer(durationText, turnOn);
  if (!timer.ok) return timer.errorMsg;

  for (size_t i : targets) {
    { CameraStateLock lock(states[i]); states[i].alertsEnabled = turnOn;
      states[i].scheduledRevertDueMs = timer.hasTimer ? timer.revertDueMs : 0;
      states[i].scheduledRevertToOn = !turnOn; }
    saveAlertEnabledPref(i, turnOn);
  }
  Serial.printf("Alerts turned %s for all %u camera(s) via %s%s.\n", turnOn ? "ON" : "OFF",
                (unsigned)targets.size(), viaWho.c_str(), timer.hasTimer ? " (timed)" : "");
  logEvent("All cameras alerts: " + String(turnOn ? "ON" : "OFF") + " via " + viaWho + timer.suffix);
  return "All " + String(targets.size()) + " camera(s) alerts: " + (turnOn ? "ON" : "OFF") + timer.suffix;
}

// Applies /on all, /off all [duration], or /snap all to every currently-
// enabled camera - see pollTelegramCommands' (telegram.h) comment on the
// "all" keyword for the (extremely narrow) trade-off it makes against a
// real camera named starting with "all". Caller (handleTelegramCommand)
// has already matched parsed.cameraName == "all" case-insensitively
// before reaching here.
static void handleAllCamerasCommand(const TelegramUser& sender, const ParsedTelegramCommand& parsed,
                                     const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  if (parsed.command == TelegramCommand::Snap) {
    std::vector<size_t> targets;
    for (size_t i = 0; i < numCameras; i++) {
      if (cameras[i].enabled) targets.push_back(i);
    }
    if (targets.empty()) {
      sendTelegramMessageTo(sender.chatId, "No enabled cameras to apply this to.");
      return;
    }
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
  String result = setAllCamerasAlertState(cameras, states, numCameras, turnOn, parsed.durationText,
                                           "Telegram (" + sender.name + ")");
  sendTelegramMessageTo(sender.chatId, result);
}

// Sent when /on, /off, or /snap arrives with no camera name at all (see
// parseTelegramCommand's own comment on the bare-command case) - one
// button per enabled camera plus "All", each carrying
// "<verb>|<cameraNameOrAll>" as its callback_data for
// handleTelegramCallbackQuery (below) to act on when tapped. No
// duration-timer support via buttons - tap-to-toggle/snap only, permanent
// on/off.
static void sendCameraPickerKeyboard(const TelegramUser& sender, TelegramCommand command,
                                      const CameraConfig cameras[], size_t numCameras) {
  String verb = commandDisplayName(command).substring(1); // "on"/"off"/"snap" - drop the leading "/"

  std::vector<std::pair<String, String>> buttons;
  for (size_t i = 0; i < numCameras; i++) {
    if (!cameras[i].enabled) continue;
    buttons.push_back({cameras[i].name, verb + "|" + cameras[i].name});
  }
  if (buttons.empty()) {
    sendTelegramMessageTo(sender.chatId, "No enabled cameras to choose from.");
    return;
  }
  buttons.push_back({"All", verb + "|all"});

  size_t skipped = 0;
  sendTelegramKeyboardTo(sender.chatId, "Choose a camera for " + commandDisplayName(command) + ":", buttons,
                          &skipped);
  if (skipped > 0) {
    // Not expected to trigger with this project's camera names (see
    // sendTelegramKeyboardTo's own comment) - but if it ever does, the
    // camera(s) missing from the keyboard above shouldn't be a silent gap
    // only visible in the Serial log.
    sendTelegramMessageTo(sender.chatId, String(skipped) + " camera name(s) were too long to show as a "
                           "button and were left off the list above - use the text command instead (e.g. "
                           "\"" + commandDisplayName(command) + " <name>\").");
  }
}

// lastUpdateId is this poll's running highest update_id, already advanced
// past `text`'s own update - passed through so the Reset case can persist
// it immediately, before ESP.restart() (see this section's top comment).
//
// text is parsed exactly once, by parseTelegramCommand (telegram_parse.h) -
// command identity, permission, and camera name are decided there and
// used as-is below, not re-derived here. The switch below has no default
// case (-Werror=switch, telegram_parse's library.json) so a new
// TelegramCommand added without a case here is a build failure, not a
// silent "unrecognized, ignored".
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
          "/on, /off, or /snap with no camera name shows a tappable button "
          "picker instead (permanent on/off/snap only, no duration timer)\n"
          "/health - board health (heap, PSRAM, NVS, WiFi signal, SD storage)\n"
          "/log [N] - the N most recent Activity log entries (default 10, max " +
          String((unsigned)EVENT_LOG_CAPACITY) + ")\n"
          "/reset - reboot the board immediately\n"
          "/help - this message\n\n"
          "<camera> matches by name or prefix; \"all\" applies to every enabled camera.\n"
          "[duration] is optional: a number of minutes (max " + String(MAX_DURATION_MINUTES) +
          "), or a 24h clock time like \"23:00\" (next occurrence - tomorrow if that time already "
          "passed today). Omitted means permanent.\n\n"
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

    case TelegramCommand::Log: {
      long count = parsed.logCountText.length() > 0 ? parsed.logCountText.toInt() : 10;
      if (count < 1) count = 10; // garbage/zero/negative falls back to the default, not an error
      if (count > (long)EVENT_LOG_CAPACITY) count = (long)EVENT_LOG_CAPACITY;

      std::vector<EventLogEntry> events = recentEvents(); // oldest-first
      String msg = "Recent activity:\n";
      if (events.empty()) {
        msg += "Nothing logged yet.";
      } else {
        // Newest first, same as webserver_activity.cpp's render - reverse
        // iterate, capped at `count`.
        long shown = 0;
        for (auto it = events.rbegin(); it != events.rend() && shown < count; ++it, ++shown) {
          msg += formatElapsedSince(it->ms, millis()) + " - " + it->text + "\n";
        }
      }
      Serial.printf("[Telegram] Replying to user \"%s\" with /log.\n", sender.name.c_str());
      sendTelegramMessageTo(sender.chatId, msg);
      return;
    }

    case TelegramCommand::Unknown:
      Serial.println("[Telegram] Unrecognized command - ignored.");
      return;
  }

  // Bare /on, /off, or /snap (no target at all) - see parseTelegramCommand's
  // own comment on why this reaches here with an empty cameraName instead
  // of Unknown. Offer an inline-keyboard picker instead of falling through
  // to the "all"/prefix-matching logic below, which would otherwise wrongly
  // treat "" as matching every camera (matchCamerasByPrefix's
  // startsWith("") is unconditionally true).
  if (parsed.cameraName.length() == 0) {
    sendCameraPickerKeyboard(sender, parsed.command, cameras, numCameras);
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

  applyOnOffToCamera(cameras[i], states[i], i, turnOn, timer, sender.name, sender.chatId);
}

// Handles an inline-keyboard button tap (sendCameraPickerKeyboard above) -
// upd.callbackData is "<verb>|<cameraNameOrAll>", e.g. "off|D01-FrontDoor".
// `sender` has already passed pollTelegramCommands' general permission
// gate, but this still re-checks the SPECIFIC permission the tapped verb
// needs (canCommand for on/off, canSnap for snap) - callback_data is
// client-supplied and never trusted alone.
static void handleTelegramCallbackQuery(const TelegramUser& sender, const TelegramUpdate& upd,
                                         const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  int sep = upd.callbackData.indexOf('|');
  String verb = sep >= 0 ? upd.callbackData.substring(0, sep) : upd.callbackData;
  String target = sep >= 0 ? upd.callbackData.substring(sep + 1) : "";

  TelegramCommand command;
  if (verb == "on") command = TelegramCommand::On;
  else if (verb == "off") command = TelegramCommand::Off;
  else if (verb == "snap") command = TelegramCommand::Snap;
  else {
    Serial.printf("[Telegram] Unrecognized callback_data \"%s\" from user \"%s\".\n",
                  upd.callbackData.c_str(), sender.name.c_str());
    answerTelegramCallback(upd.callbackQueryId, "Unrecognized action.");
    return;
  }

  bool authorized = (command == TelegramCommand::Snap) ? sender.canSnap : sender.canCommand;
  if (!authorized) {
    Serial.printf("[Telegram] User \"%s\" not authorized for the %s button.\n", sender.name.c_str(), verb.c_str());
    answerTelegramCallback(upd.callbackQueryId, "Not authorized.");
    sendTelegramMessageTo(sender.chatId, "You're not authorized to use " + commandDisplayName(command) + ".");
    return;
  }

  String targetLower = target;
  targetLower.toLowerCase();
  if (targetLower == "all") {
    // Reuses handleAllCamerasCommand as-is - it only reads parsed.command
    // (and parsed.durationText, always "" here - buttons are permanent
    // only), never parsed.cameraName, so a synthetic ParsedTelegramCommand
    // built just for this call is safe.
    ParsedTelegramCommand parsed;
    parsed.command = command;
    parsed.requiredPermission = requiredPermissionForCommand(command);
    handleAllCamerasCommand(sender, parsed, cameras, states, numCameras);
    answerTelegramCallback(upd.callbackQueryId, "");
    return;
  }

  // Exact match, not matchCamerasByPrefix - the button's label was this
  // camera's real name, generated by sendCameraPickerKeyboard itself, not
  // typed by hand, so there's no prefix-ambiguity case to handle here.
  // Still requires cameras[i].enabled, same as matchCamerasByPrefix does
  // for the text-command path - the camera list can change between the
  // picker being sent and a button being tapped, and a stale button for a
  // camera that's since been disabled must not silently still apply.
  int idx = -1;
  for (size_t i = 0; i < numCameras; i++) {
    if (cameras[i].enabled && cameras[i].name.equalsIgnoreCase(target)) { idx = (int)i; break; }
  }
  if (idx < 0) {
    // The camera list can change between the picker being sent and a
    // button being tapped (renamed/deleted/disabled, reboot required to
    // apply - see webserver_cameras.cpp) - handled as a clean "no longer
    // available" reply, not a crash.
    Serial.printf("[Telegram] Callback target camera \"%s\" no longer available (user \"%s\").\n",
                  target.c_str(), sender.name.c_str());
    answerTelegramCallback(upd.callbackQueryId, "That camera is no longer available.");
    sendTelegramMessageTo(sender.chatId, "\"" + target + "\" is no longer available - it may have been "
                                          "renamed, deleted, or disabled since this button was sent.");
    return;
  }

  if (command == TelegramCommand::Snap) {
    Serial.printf("[%s] On-demand snapshot requested by user \"%s\" via button.\n",
                  cameras[idx].name.c_str(), sender.name.c_str());
    sendOnDemandSnapshot(cameras[idx], states[idx], sender.chatId);
    answerTelegramCallback(upd.callbackQueryId, "");
    return;
  }

  bool turnOn = (command == TelegramCommand::On);
  AlertTimer permanent; // default-constructed: ok=true, hasTimer=false, suffix="" - buttons are permanent only
  applyOnOffToCamera(cameras[idx], states[idx], (size_t)idx, turnOn, permanent, sender.name, sender.chatId);
  answerTelegramCallback(upd.callbackQueryId, "");
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
  String body;
  {
    // Scoped tightly to the network fetch only, released BEFORE the
    // update-dispatch loop below - that loop calls handleTelegramCommand/
    // handleTelegramCallbackQuery, which can themselves call back into
    // sendTelegramMessageTo/sendOnDemandSnapshot and so re-acquire
    // g_telegramNetMutex. xSemaphoreCreateMutex() is non-recursive -
    // holding the lock across that loop would have this same task block
    // trying to re-take a mutex it already owns. See g_telegramNetMutex's
    // own comment for the mutex's overall purpose.
    TelegramNetLock netLock;
    if (!netLock.held()) {
      Serial.println("[Telegram] pollTelegramCommands: timed out waiting for Telegram send capacity - "
                      "skipping this poll.");
      return; // transient - next poll (TELEGRAM_COMMAND_POLL_MS) retries
    }

    WiFiClientSecure client;
    client.setCACert(TELEGRAM_ROOT_CA);
    client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000); // seconds, not ms - see sendTelegramPhotoBuffered's comment

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
    body = http.getString();
    http.end();
  } // netLock released here - everything below runs unlocked, on purpose

  String parseError;
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body, &parseError);
  if (parseError.length() > 0) {
    Serial.printf("[Telegram] pollTelegramCommands: %s\n", parseError.c_str());
  }

  std::vector<TelegramUser> users = loadTelegramUsers();
  for (auto& upd : updates) {
    // Advanced in RAM for every update (so the same batch isn't refetched
    // next poll), but NOT persisted to NVS here - Telegram delivers every
    // inbound message regardless of sender, so persisting per-update would
    // let anyone who finds this bot force an NVS write with no permission
    // required. Persisted once at the end of this loop instead (see this
    // section's top comment).
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
        recordUnknownChat(upd.chatId);
      }
      continue;
    }

    if (upd.hasCallbackQuery) {
      handleTelegramCallbackQuery(*sender, upd, cameras, states, numCameras);
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
