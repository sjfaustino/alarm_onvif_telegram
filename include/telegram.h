#pragma once
#include <Arduino.h>
#include "config.h"
#include "camera.h"

// Fetches a snapshot JPEG from st.snapshotUri, sends it to Telegram with a
// caption of "<camera name> - <UTC timestamp>", subject to cfg's per-camera
// alert cooldown. Safe to call on every motion event; it self-throttles.
void triggerMotionAlert(const CameraConfig& cfg, CameraState& st);

// Sends a plain text message to TELEGRAM_CHAT_ID (no photo attached) - used
// for the boot-time "camera monitor is online" notice, but generic enough
// for any future status/error message. Returns false on any failure
// (connect, write, or a non-200 Telegram response).
bool sendTelegramMessage(const String& text);

// True once TELEGRAM_ROOT_CA (telegram_ca.h) has been filled in with a real
// certificate. False means it's still the placeholder - every Telegram send
// will fail TLS verification until it's replaced. Checked once at boot so
// this is diagnosed with a clear log line instead of a stream of opaque
// "Could not connect" errors.
bool telegramCAConfigured();
