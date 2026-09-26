#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment

// Multipart/form-data request builder for Telegram's sendPhoto endpoint,
// split out of the Telegram send path (telegram_transport.cpp) so it can be unit-tested natively
// (test/test_telegram_multipart) without a live bot token or network.
// Shared by both the streamed and buffered send paths so they can't drift
// apart.
struct TelegramMultipart {
  String boundary, head, tail, requestLine;
  size_t contentLength;
};

// jpgLen only feeds contentLength - the JPEG bytes themselves aren't part
// of this (streamed separately by the caller, see sendTelegramPhotoBuffered
// in telegram_transport.cpp). botToken is passed in rather than read from a global so
// this stays pure/testable without a real secrets.h value.
TelegramMultipart buildMultipart(size_t jpgLen, const String& caption, const String& chatId,
                                  const char* botToken);

// Same shape as buildMultipart above, but for Telegram's sendDocument
// endpoint instead of sendPhoto - an arbitrary file attachment (the "/backup"
// command's config-export text file, telegram_commands.cpp) rather than always a
// JPEG. fileLen only feeds contentLength, same reasoning as jpgLen above;
// the file bytes themselves are streamed separately by the caller.
// contentType defaults to "text/plain" - the only kind this project sends
// today - but is a parameter rather than hardcoded, matching this being a
// generic document builder, not a config-export-specific one.
TelegramMultipart buildDocumentMultipart(size_t fileLen, const String& caption, const String& chatId,
                                          const char* botToken, const String& filename,
                                          const String& contentType = "text/plain");
