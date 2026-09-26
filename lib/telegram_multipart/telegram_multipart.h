#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment

// multipart/form-data framing for sendPhoto/sendDocument, tested natively.
struct TelegramMultipart {
  String boundary, head, tail, requestLine;
  size_t contentLength;
};

// Framing only: the caller streams the JPEG between head and tail; jpgLen only
// feeds contentLength.
TelegramMultipart buildMultipart(size_t jpgLen, const String& caption, const String& chatId,
                                  const char* botToken);

// Same for sendDocument (the /backup file).
TelegramMultipart buildDocumentMultipart(size_t fileLen, const String& caption, const String& chatId,
                                          const char* botToken, const String& filename,
                                          const String& contentType = "text/plain");
