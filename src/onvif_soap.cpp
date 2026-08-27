#include "onvif_soap.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"

String makeUUID() {
  uint32_t a = esp_random(), b = esp_random(), c = esp_random(), d = esp_random();
  char buf[80];
  snprintf(buf, sizeof(buf), "urn:uuid:%08lx-%04lx-%04lx-%04lx-%08lx%04lx",
           (unsigned long)a, (unsigned long)((b >> 16) & 0xFFFF), (unsigned long)(b & 0xFFFF),
           (unsigned long)((c >> 16) & 0xFFFF), (unsigned long)c, (unsigned long)(d & 0xFFFF));
  return String(buf);
}

// Deliberately gmtime_r, not Arduino's getLocalTime()/localtime_r - this
// feeds WS-Security's Created timestamp (see makeSecurityHeader below),
// which ONVIF requires to be true UTC. getLocalTime() would instead return
// whatever main.cpp's setupTime() set TZ to - a DST-aware local time, if a
// POSIX TZ rule is configured there for Telegram caption display (see
// network_store.h's comment on WifiCredentials::posixTz) - which would
// silently mislabel a shifted time as "Z" (UTC) and could fail a camera's
// own timestamp-freshness check. Mirrors getLocalTime()'s own sync-detection
// logic (tm_year > 116 means the clock has actually been set by SNTP, not
// still sitting at the 1970 epoch) so the "wait up to 2s for sync" behavior
// is unchanged.
String isoTimeNow() {
  time_t now = 0;
  struct tm timeinfo = {};
  uint32_t start = millis();
  do {
    time(&now);
    gmtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (2016 - 1900)) break;
    delay(10);
  } while (millis() - start <= 2000);

  if (timeinfo.tm_year <= (2016 - 1900)) {
    Serial.println("ERROR: NTP time unavailable");
    return "";
  }

  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

static void randomBytes(uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i += 4) {
    uint32_t r = esp_random();
    size_t copyLen = min((size_t)4, len - i);
    memcpy(data + i, &r, copyLen);
  }
}

bool sha1Bytes(const uint8_t* input, size_t inputLen, uint8_t output[20]) {
  mbedtls_sha1_context ctx;
  mbedtls_sha1_init(&ctx);
  bool ok = mbedtls_sha1_starts(&ctx) == 0 &&
            mbedtls_sha1_update(&ctx, input, inputLen) == 0 &&
            mbedtls_sha1_finish(&ctx, output) == 0;
  mbedtls_sha1_free(&ctx);
  return ok;
}

String base64Encode(const uint8_t* data, size_t len) {
  size_t outputLen = 0;
  size_t required = ((len + 2) / 3) * 4 + 1;
  unsigned char* output = (unsigned char*)malloc(required);
  if (!output) return "";
  int result = mbedtls_base64_encode(output, required, &outputLen, data, len);
  if (result != 0) { free(output); return ""; }
  output[outputLen] = 0;
  String s = String((char*)output);
  free(output);
  return s;
}

// PasswordDigest = Base64(SHA1(nonce(binary) + Created + Password))
String makeSecurityHeader(const char* user, const char* pass) {
  uint8_t nonce[16];
  randomBytes(nonce, sizeof(nonce));
  String nonceB64 = base64Encode(nonce, sizeof(nonce));
  String created = isoTimeNow();
  if (created.length() == 0) return "";

  size_t totalLen = sizeof(nonce) + created.length() + strlen(pass);
  uint8_t* input = (uint8_t*)malloc(totalLen);
  if (!input) { Serial.println("ERROR: Cannot allocate WSSE buffer"); return ""; }

  size_t pos = 0;
  memcpy(input + pos, nonce, sizeof(nonce)); pos += sizeof(nonce);
  memcpy(input + pos, created.c_str(), created.length()); pos += created.length();
  memcpy(input + pos, pass, strlen(pass));

  uint8_t digest[20];
  bool ok = sha1Bytes(input, totalLen, digest);
  free(input);
  if (!ok) { Serial.println("ERROR: SHA1 failed"); return ""; }

  String digestB64 = base64Encode(digest, sizeof(digest));

  String xml;
  xml += "<wsse:Security>";
  xml += "<wsse:UsernameToken>";
  xml += "<wsse:Username>" + xmlEscape(user) + "</wsse:Username>";
  xml += "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">";
  xml += digestB64;
  xml += "</wsse:Password>";
  xml += "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">";
  xml += nonceB64;
  xml += "</wsse:Nonce>";
  xml += "<wsu:Created>" + created + "</wsu:Created>";
  xml += "</wsse:UsernameToken>";
  xml += "</wsse:Security>";
  return xml;
}

String soapEnvelope(const String& action, const String& body, const String& to,
                     const char* user, const char* pass, bool includeReplyToAnonymous,
                     bool useWSSecurity) {
  String messageID = makeUUID();
  String xml;
  xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  xml += "<s:Envelope "
         "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
         "xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" "
         "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" "
         "xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\" "
         "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" "
         "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
         "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
         "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
         "xmlns:tns1=\"http://www.onvif.org/ver10/topics\" "
         "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
         "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-wssecurity-utility-1.0.xsd\">";
  xml += "<s:Header>";
  xml += "<wsa:Action>" + xmlEscape(action) + "</wsa:Action>";
  xml += "<wsa:MessageID>" + messageID + "</wsa:MessageID>";
  if (includeReplyToAnonymous) {
    xml += "<wsa:ReplyTo><wsa:Address>"
           "http://www.w3.org/2005/08/addressing/anonymous"
           "</wsa:Address></wsa:ReplyTo>";
  }
  if (to.length() > 0) {
    xml += "<wsa:To>" + xmlEscape(to) + "</wsa:To>";
  }
  if (useWSSecurity) {
    xml += makeSecurityHeader(user, pass);
  }
  xml += "</s:Header>";
  xml += "<s:Body>" + body + "</s:Body>";
  xml += "</s:Envelope>";
  return xml;
}

static String shortAction(const String& action) {
  int lastSlash = action.lastIndexOf('/');
  return (lastSlash >= 0) ? action.substring(lastSlash + 1) : action;
}

String soapPost(const char* cameraName, const String& url, const String& action, const String& xml,
                const char* basicAuthUser, const char* basicAuthPass) {
  if (VERBOSE_SOAP_LOG) {
    Serial.println("\n---- SOAP POST ----");
    Serial.println(url);
    Serial.println(action);
    Serial.println(xml);
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    Serial.println("ERROR: http.begin() failed for " + url);
    return "";
  }

  if (basicAuthUser != nullptr && basicAuthPass != nullptr) {
    http.setAuthorization(basicAuthUser, basicAuthPass);
  }

  String contentType = "application/soap+xml; charset=utf-8; action=\"" + action + "\"";
  http.addHeader("Content-Type", contentType);
  http.addHeader("Connection", "close");

  int code = http.POST((uint8_t*)xml.c_str(), xml.length());
  String response = (code > 0) ? http.getString() : "";
  bool isFault = responseHasFault(response);

  bool suppress = SUPPRESS_SOAP_SUCCESS_LOG && code == 200 && !isFault;
  if (!suppress) {
    Serial.printf("[%s] [SOAP] %-30s HTTP=%-4d len=%-5u%s\n",
                  cameraName, shortAction(action).c_str(), code, (unsigned)response.length(),
                  isFault ? "  <-- FAULT" : "");
  }

  // Negative codes never reach a server at all (TCP/DNS/timeout-level
  // failure) - HTTPClient::errorToString turns e.g. -1 into "connection
  // refused" instead of leaving you to look up the number.
  if (code < 0) {
    Serial.printf("       ^ %s\n", HTTPClient::errorToString(code).c_str());
  }

  if (VERBOSE_SOAP_LOG || isFault) {
    Serial.println("---- RESPONSE ----");
    Serial.println(response);
  }

  http.end();
  return response;
}

