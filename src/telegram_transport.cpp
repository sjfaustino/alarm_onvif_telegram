#include "telegram.h"
#include "telegram_internal.h"
#include "config.h"
#include "telegram_ca.h"
#include "telegram_users.h"
#include "telegram_i18n.h"
#include "telegram_multipart.h"
#include "telegram_retry_queue.h"
#include "event_log_store.h"
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include <vector>

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
TelegramNetLock::TelegramNetLock()
    : held_(xSemaphoreTake(g_telegramNetMutex, pdMS_TO_TICKS(TELEGRAM_NET_MUTEX_TIMEOUT_MS)) == pdTRUE) {}
TelegramNetLock::~TelegramNetLock() { if (held_) xSemaphoreGive(g_telegramNetMutex); }

// Non-blocking peek at g_telegramNetMutex - true if some task is currently
// mid-send (or waiting for its own turn), without waiting or taking a turn
// itself. For a heavy, memory-hungry, user-triggered, delay-tolerable
// background job that doesn't itself send anything through Telegram (WS-
// Discovery's multi-second UDP listen, Test All's per-camera SOAP burst -
// see webserver_cameras.cpp's startCameraDiscoveryAsync/
// startTestAllCamerasAsync) to defer starting rather than risk its own
// buffers/sockets overlapping an in-flight photo send's JPEG+TLS buffers -
// exactly the class of coincidence g_telegramNetMutex's own comment
// describes a real field incident from, just between Telegram sends and a
// DIFFERENT subsystem instead of two Telegram sends against each other.
bool telegramSendInProgress() {
  if (xSemaphoreTake(g_telegramNetMutex, 0) == pdTRUE) {
    xSemaphoreGive(g_telegramNetMutex);
    return false;
  }
  return true;
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

  int lineEnd = fullResponse.indexOf('\n');
  if (lineEnd < 0) {
    // No newline at all - a malformed/truncated response, not a real HTTP
    // status line. String::substring(0, -1) would otherwise clamp to the
    // WHOLE response and scan the entire body for "200", which could match
    // incidentally inside body text (e.g. a chat ID or byte count) and
    // misreport a failed send as successful. Treated as a failure, same as
    // the "no response within timeout" case above.
    Serial.println("Telegram sendPhoto FAILED: no status line in response.");
    return false;
  }
  String statusLine = fullResponse.substring(0, lineEnd);
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

// Same shape as sendTelegramPhotoBuffered above, but for Telegram's
// sendDocument endpoint (buildDocumentMultipart) - an arbitrary text file
// already fully in RAM, for the /backup command's config-export
// attachment. Single attempt, no retry-on-failure counterpart the way
// photos have (sendTelegramPhotoWithRetry below) - /backup is an
// explicit, low-frequency admin action the sender can just re-issue by
// hand if it fails, unlike a motion alert that might never get another
// chance.
bool sendTelegramDocumentBuffered(const String& content, const String& filename, const String& caption,
                                          const String& chatId) {
  if (content.length() == 0) return false;

  // See g_telegramNetMutex's own comment.
  TelegramNetLock netLock;
  if (!netLock.held()) {
    Serial.println("Telegram sendDocument: timed out waiting for Telegram send capacity - skipping.");
    return false;
  }

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
    return false;
  }

  TelegramMultipart m = buildDocumentMultipart(content.length(), caption, chatId, TELEGRAM_BOT_TOKEN, filename);

  size_t sent = 0;
  sent += writeAllBytes(client, (const uint8_t*)m.requestLine.c_str(), m.requestLine.length());
  sent += writeAllBytes(client, (const uint8_t*)m.head.c_str(), m.head.length());
  sent += writeAllBytes(client, (const uint8_t*)content.c_str(), content.length());
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
bool sendTelegramPhotoWithRetry(const uint8_t* jpg, size_t jpgLen, const String& caption,
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
String nowTimestampString() {
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
bool localClockSynced() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  return tmStruct.tm_year > (2016 - 1900);
}

int currentLocalMinuteOfDay() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  return tmStruct.tm_hour * 60 + tmStruct.tm_min;
}

String formatLocalClockTime(unsigned long dueMs) {
  if (!localClockSynced()) return "";
  // dueMs and millis() share the same monotonic clock, so their
  // difference is a real elapsed duration regardless of what wall-clock
  // time happens to be right now - added onto the current epoch time to
  // get the epoch dueMs actually corresponds to.
  long offsetSec = (long)(dueMs - millis()) / 1000;
  time_t now; time(&now);
  time_t due = now + offsetSec;
  struct tm tmStruct; localtime_r(&due, &tmStruct);
  String hh = String(tmStruct.tm_hour); if (hh.length() < 2) hh = "0" + hh;
  String mm = String(tmStruct.tm_min);  if (mm.length() < 2) mm = "0" + mm;
  return hh + ":" + mm;
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

bool sendTelegramMessageTo(const String& chatId, const String& text) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = text;
  return sendTelegramApiCall("sendMessage", doc);
}

bool sendTelegramMessageToChatId(const String& chatId, const String& text) {
  return sendTelegramMessageTo(chatId, text);
}

// Sends text with an inline keyboard, one button per row - `buttons` is
// (label, callback_data) pairs. handleTelegramCallbackQuery (below)
// receives a tapped button's callback_data back on the next poll. Skips
// (and logs) any button whose callback_data would exceed Telegram's
// 64-byte limit rather than sending a broken button - not expected to
// trigger with this project's camera names, but if `outSkipped` is
// non-null it's set to how many were dropped, so the caller can still
// tell the user instead of the gap being silent past the Serial log.
bool sendTelegramKeyboardTo(const String& chatId, const String& text,
                                    const std::vector<std::pair<String, String>>& buttons,
                                    size_t* outSkipped) {
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
bool answerTelegramCallback(const String& callbackQueryId, const String& text) {
  JsonDocument doc;
  doc["callback_query_id"] = callbackQueryId;
  if (text.length() > 0) doc["text"] = text;
  return sendTelegramApiCall("answerCallbackQuery", doc);
}

// Broadcasts to every Telegram user with systemMessages enabled - used for
// the heartbeat, the boot-online notice, and the "no credentials" fatal
// alert. Returns true if it reached at least one recipient.
bool sendTelegramMessage(std::function<String(TelegramLang)> compose) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  bool anyRecipient = false;
  bool anyOk = false;
  for (auto& u : users) {
    if (!u.systemMessages) continue;
    anyRecipient = true;
    String text = compose(u.language);
    if (sendTelegramMessageTo(u.chatId, text)) {
      anyOk = true;
    } else {
      // Most likely WAN was down at the exact moment this broadcast went
      // out (e.g. a watchdog's own OutageDetected alert - the worst
      // possible moment to try sending) - queue it for a retry once
      // connectivity is back instead of losing it silently. See
      // telegram_retry_queue.h's own comment for what this deliberately
      // does NOT cover (command replies, photos).
      enqueueFailedTelegramMessage(u.chatId, text);
    }
    // Each recipient can independently block on g_telegramNetMutex for up
    // to TELEGRAM_NET_MUTEX_TIMEOUT_MS (45s) before a send is even
    // attempted - with several systemMessages recipients configured, this
    // loop alone can exceed the 90s task watchdog timeout on loop()'s task
    // (main.cpp), before the caller (sendHeartbeat, checkScheduledAlertReverts,
    // checkCameraOnlineStatus, ...) ever gets a chance to return and feed
    // it itself. A no-op (harmless ESP_ERR_NOT_FOUND, ignored like every
    // other reset call in this project) when called from a camera task,
    // which was never subscribed to this watchdog in the first place - see
    // initWatchdog()'s own comment (main.cpp).
    esp_task_wdt_reset();
  }
  if (!anyRecipient) {
    Serial.println("sendTelegramMessage: no Telegram user has systemMessages enabled - nothing sent.");
    return false;
  }
  return anyOk;
}

