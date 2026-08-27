#pragma once
#include <Arduino.h>
#include "secrets.h"

// WiFi and Telegram credentials live in secrets.h (gitignored). Nothing
// sensitive belongs in this file, since this one IS meant to be committed.
//
// Cameras are managed at runtime - see camera_store.h (CameraConfig, NVS
// load/save) and webserver.h (the web UI). secrets.h's CAMERA_SEED only
// seeds NVS on first boot; after that the web UI is the only way to
// change them.

// ============================================================
// Timing (all in ms unless noted)
// ============================================================
static const unsigned long PULL_INTERVAL_MS         = 2000UL;        // per-camera poll cadence you asked for
static const unsigned long SUBSCRIPTION_LIFETIME_MS = 4UL * 60UL * 1000UL;
static const unsigned long RENEW_MARGIN_MS          = 60UL * 1000UL;
static const unsigned long RETRY_INTERVAL_MS        = 10000UL;
static const uint16_t      HTTP_TIMEOUT_MS          = 10000;
static const unsigned long HEARTBEAT_INTERVAL_MS    = 6UL * 60UL * 60UL * 1000UL; // liveness ping cadence
static const unsigned long TELEGRAM_COMMAND_POLL_MS = 5000UL;        // /on, /off, /status polling cadence
static const size_t        SNAPSHOT_MAX_BYTES       = 100000;        // internal-RAM fallback cap - see note below
static const size_t        SNAPSHOT_MAX_BYTES_PSRAM = 2000000UL;     // PSRAM buffer cap - generous; real snapshots are far smaller
static const bool          VERBOSE_SOAP_LOG         = false;         // flip true to debug one camera at a time

// PSRAM is a hard requirement - setup() refuses to start if
// ESP.getPsramSize() is 0, since a snapshot alert going to multiple
// Telegram users needs the JPEG buffered once in RAM and resent per
// recipient. SNAPSHOT_MAX_BYTES is only telegram.cpp's fallback cap for
// the rare case a PSRAM allocation itself fails (fragmentation).
//
// camera.cpp's per-camera FreeRTOS tasks are pinned to core 1, requiring a
// genuine second core - would need plain xTaskCreate if ever flashed to a
// single-core chip.
