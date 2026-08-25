#pragma once
#include <Arduino.h>

String makeUUID();
String isoTimeNow();
String base64Encode(const uint8_t* data, size_t len);
bool   sha1Bytes(const uint8_t* input, size_t inputLen, uint8_t output[20]);
String xmlEscape(const String& value);
String makeSecurityHeader(const char* user, const char* pass);

// Builds a full SOAP envelope. `to` is the wsa:To value - pass the device
// service, event service, or pull-point URL depending on which call this is.
// If useWSSecurity is false, no <wsse:Security> header is added - the
// caller is expected to pass basicAuthUser/Pass to soapPost instead.
String soapEnvelope(const String& action, const String& body, const String& to,
                     const char* user, const char* pass, bool includeReplyToAnonymous,
                     bool useWSSecurity = true);

// basicAuthUser/basicAuthPass: set both (non-null) to send HTTP Basic Auth
// instead of/alongside WS-Security - needed for cameras with useWSSecurity=false.
String soapPost(const String& url, const String& action, const String& xml,
                 const char* basicAuthUser = nullptr, const char* basicAuthPass = nullptr);

String findElementByLocalName(const String& xml, const String& localName, int fromPosition = 0);
String findAttributeValue(const String& xml, const String& elementName, const String& attributeName);
bool   responseHasFault(const String& response);
