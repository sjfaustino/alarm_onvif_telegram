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
// Hides the routine "[camera] [SOAP] Action HTTP=200 len=N" line for a
// call that succeeded and wasn't a SOAP fault - most of the serial
// output during steady-state polling. Faults and non-200/negative codes
// always print regardless, so nothing actionable gets hidden. Flip to
// false to see every call again (e.g. while troubleshooting timing).
static const bool          SUPPRESS_SOAP_SUCCESS_LOG = true;

// PSRAM is a hard requirement - setup() refuses to start if
// ESP.getPsramSize() is 0, since a snapshot alert going to multiple
// Telegram users needs the JPEG buffered once in RAM and resent per
// recipient. SNAPSHOT_MAX_BYTES is only telegram.cpp's fallback cap for
// the rare case a PSRAM allocation itself fails (fragmentation).
//
// camera.cpp's per-camera FreeRTOS tasks are pinned to core 1, requiring a
// genuine second core - would need plain xTaskCreate if ever flashed to a
// single-core chip.

// ============================================================
// Optional SD card storage (sd_store.h/.cpp) - a generic SPI microSD
// breakout module, entirely optional and off by default (see
// SdSettings::enabled, dashboard Storage page). When enabled AND a card
// is actually detected at boot, snapshot history (webserver.cpp's
// /cameras/snapshot, the Cameras page's preview strip) is stored here
// instead of the small PSRAM ring, with far more history retained and
// persisting across reboots. If disabled, or enabled but no module/card
// responds at boot, monitoring is entirely unaffected - snapshot history
// just falls back to the existing PSRAM ring, exactly as it already
// works today.
//
// *** VERIFY AND ADJUST these for your actual wiring before flashing ***
// These are common ESP32-S3 default SPI2 pins, not guaranteed for your
// specific dev board - check your board's pinout/datasheet. Nothing else
// in this project uses SPI, so any four free GPIOs work; these are just a
// reasonable starting point.
static const int SD_CS_PIN   = 10;
static const int SD_SCK_PIN  = 12;
static const int SD_MISO_PIN = 13;
static const int SD_MOSI_PIN = 11;

// Safety margin kept free on the card at all times - a write that would
// drop free space below this triggers pruning first (see sd_store.cpp's
// writeSnapshot). Not precisely tuned to any card size on purpose: large
// enough to comfortably fit several more snapshots plus filesystem
// overhead, small enough not to waste meaningful capacity on any card
// worth using for this.
static const uint64_t SD_FREE_SPACE_RESERVE_BYTES = 50UL * 1024UL * 1024UL; // 50MB

// Per-camera fairness ceiling, independent of the free-space reserve
// above - without this, one chatty camera could fill most of the card
// and crowd out a quiet camera's retained history, since pruning is
// deliberately per-camera (see sd_store.cpp) rather than a global,
// cross-directory walk.
static const size_t SD_MAX_FILES_PER_CAMERA = 300;

// Caps how many files a single write's prune pass deletes, even if that
// isn't enough to clear the reserve/ceiling above - bounds how long one
// write can hold the SD mutex (blocking every other camera's own writes)
// during pruning. If one call's cap isn't enough, the next write's own
// prune pass continues the job.
static const size_t SD_PRUNE_MAX_FILES_PER_WRITE = 5;

// How long waitForSdIdle() (sd_store.h) waits for an in-flight SD
// operation to finish before a deliberate reboot proceeds anyway.
// Generous enough to cover a legitimately slow erase-all pass across
// several cameras' worth of files on a slow card, short enough that a
// genuinely wedged SD operation doesn't leave someone who pressed
// "reboot" stuck waiting indefinitely.
static const unsigned long SD_IDLE_WAIT_TIMEOUT_MS = 10000UL;

// Clamp for the dashboard's "automatic full storage check" interval
// (SdSettings::checkIntervalHours, Storage page) - just a sanity bound on
// the number field, same idea as CameraConfig's snapshotBurstCount clamp.
// 720h = 30 days.
static const uint32_t SD_CHECK_INTERVAL_MAX_HOURS = 720;
