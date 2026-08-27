#include "telegram_multipart.h"
#include <cstring>

TelegramMultipart buildMultipart(size_t jpgLen, const String& caption, const String& chatId,
                                  const char* botToken) {
  TelegramMultipart m;
  m.boundary = "----ESP32Boundary7MA4YWxk";
  m.head.reserve(160 + caption.length());
  m.head += "--" + m.boundary + "\r\n";
  m.head += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + chatId + "\r\n";
  m.head += "--" + m.boundary + "\r\n";
  m.head += "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n";
  m.head += "--" + m.boundary + "\r\n";
  m.head += "Content-Disposition: form-data; name=\"photo\"; filename=\"snap.jpg\"\r\n";
  m.head += "Content-Type: image/jpeg\r\n\r\n";
  m.tail = "\r\n--" + m.boundary + "--\r\n";
  m.contentLength = m.head.length() + jpgLen + m.tail.length();

  m.requestLine.reserve(96 + strlen(botToken));
  m.requestLine += "POST /bot" + String(botToken) + "/sendPhoto HTTP/1.1\r\n";
  m.requestLine += "Host: api.telegram.org\r\n";
  m.requestLine += "Content-Type: multipart/form-data; boundary=" + m.boundary + "\r\n";
  m.requestLine += "Content-Length: " + String((unsigned long)m.contentLength) + "\r\n";
  m.requestLine += "Connection: close\r\n\r\n";
  return m;
}
