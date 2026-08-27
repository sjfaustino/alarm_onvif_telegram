#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment

// Multipart/form-data request builder for Telegram's sendPhoto endpoint,
// split out of telegram.cpp so it can be unit-tested natively
// (test/test_telegram_multipart) without a live bot token or network.
// Shared by both the streamed and buffered send paths so they can't drift
// apart.
struct TelegramMultipart {
  String boundary, head, tail, requestLine;
  size_t contentLength;
};

// jpgLen only feeds contentLength - the JPEG bytes themselves aren't part
// of this (streamed separately by the caller, see sendTelegramPhotoBuffered
// in telegram.cpp). botToken is passed in rather than read from a global so
// this stays pure/testable without a real secrets.h value.
TelegramMultipart buildMultipart(size_t jpgLen, const String& caption, const String& chatId,
                                  const char* botToken);
