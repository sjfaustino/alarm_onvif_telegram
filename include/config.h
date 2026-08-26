#pragma once
#include <Arduino.h>
#include "secrets.h"

// WiFi and Telegram credentials live in secrets.h (gitignored - see
// secrets.h.example for the template). Nothing sensitive belongs in this
// file, since this one IS meant to be committed.
//
// Camera definitions used to live here as a compile-time CAMERAS[] array,
// matched to secrets.h's CAMERA_SECRETS[] by name. They're now managed at
// runtime instead - see camera_store.h (the CameraConfig struct and NVS
// load/save) and webserver.h (the add/delete/view web UI). secrets.h still
// has a one-time CAMERA_SEED array for migrating already-tuned cameras into
// NVS on first boot; after that, the web UI is the only way to change them.

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

// NOTE ON HARDWARE: earlier in development this project also ran on a
// couple of no-PSRAM boards (an ESP32-S2, a classic dual-core ESP32).
// PSRAM is now a hard requirement instead - main.cpp's setup() refuses to
// start at all if ESP.getPsramSize() is 0, rather than run degraded (see
// its comment for why: a snapshot alert can go to multiple Telegram users,
// so the JPEG has to be buffered in RAM once and resent per recipient).
// SNAPSHOT_MAX_BYTES only still exists as telegram.cpp's fallback cap for
// the rare case a PSRAM allocation itself fails (fragmentation) - see
// allocateSnapshotBuffer() there.
//
// Core count: camera.cpp's per-camera FreeRTOS tasks are pinned to core 1
// in main.cpp, which requires a genuine second core - every PSRAM-equipped
// ESP32-S3 variant this targets is dual-core, so this hasn't come up as a
// real constraint, but would need changing to plain xTaskCreate (no
// pinning) if this were ever flashed to a single-core chip.
// mbedTLS's ~16KB RX + 16KB TX TLS session buffers are fixed at build time
// regardless of which ESP32 this is.
