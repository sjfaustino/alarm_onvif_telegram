#pragma once
#include <Arduino.h>
#include "secrets.h"

// Project-wide constants. Credentials live in secrets.h (gitignored); cameras
// live in NVS and are edited from the dashboard (secrets.h's CAMERA_SEED only
// seeds a fresh board).

// FIRMWARE_VERSION is in build_version.h so a per-build value doesn't force a
// rebuild of everything that includes this file.

// ============================================================
// Timing (all in ms unless noted)
// ============================================================
// PULL_INTERVAL_MS is only CameraConfig::pollIntervalMs's default; camera.cpp
// reads the per-camera value.
static const unsigned long PULL_INTERVAL_MS         = 2000UL;
static const unsigned long SUBSCRIPTION_LIFETIME_MS = 4UL * 60UL * 1000UL;
static const unsigned long RENEW_MARGIN_MS          = 60UL * 1000UL;
static const unsigned long RETRY_INTERVAL_MS        = 10000UL;
// Consecutive unrecognized PullMessages responses before a forced resubscribe
// (~10s at the default 2s poll).
static const uint8_t        PULL_MESSAGES_AMBIGUOUS_LIMIT = 5;
// Reserved up front for g_cameras/g_cameraStates so adding a camera live never
// reallocates them; tasks hold raw pointers into both (see camera_tasks.h).
static const size_t MAX_CAMERAS = 24;

// Poll interval clamp. The floor matters: every SOAP call opens a fresh
// connection ("Connection: close"), and some cameras only accept 1-2 at once.
static const unsigned long CAMERA_POLL_INTERVAL_MIN_MS = 250UL;
static const unsigned long CAMERA_POLL_INTERVAL_MAX_MS = 30000UL; // 30s
// Retry cadence for a subscribed camera whose snapshot URI isn't resolved yet.
// Well above the poll rate: a camera with no Media service fails this forever.
static const unsigned long SNAPSHOT_URI_RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 minutes
static const uint16_t      HTTP_TIMEOUT_MS          = 10000;
static const unsigned long HEARTBEAT_INTERVAL_MS    = 6UL * 60UL * 60UL * 1000UL; // liveness ping cadence
// Fixed interval from boot, not a time of day - avoids depending on a synced
// clock.
static const unsigned long DAILY_DIGEST_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL; // 24 hours
static const unsigned long TELEGRAM_COMMAND_POLL_MS = 5000UL;        // /on, /off, /status polling cadence
// Bounded wait for the Telegram TLS mutex (see telegram_transport.cpp).
// Outlasts one queued send (~40s) but stays under RENEW_MARGIN_MS, since a
// camera task waiting here can't renew its subscription.
static const unsigned long TELEGRAM_NET_MUTEX_TIMEOUT_MS = 45000UL;

// Retry queue for broadcast alerts that failed (usually a WAN outage). Oldest
// entries drop when full; entries older than MAX_AGE are no longer worth
// sending.
static const size_t TELEGRAM_RETRY_QUEUE_CAPACITY = 20;
static const unsigned long TELEGRAM_RETRY_FLUSH_INTERVAL_MS = 60UL * 1000UL;      // 1 minute
static const unsigned long TELEGRAM_RETRY_MAX_AGE_MS = 24UL * 60UL * 60UL * 1000UL; // 24 hours

// /restore: how long a bare "/restore" waits for the file, and a size cap. The
// file is buffered in internal heap, so the cap stays near a real export's
// size.
static const unsigned long RESTORE_PENDING_WINDOW_MS = 5UL * 60UL * 1000UL; // 5 minutes
static const size_t RESTORE_MAX_FILE_BYTES = 64UL * 1024UL; // 64KB
// NVS usage only grows from dashboard edits, so hourly is plenty.
static const unsigned long NVS_USAGE_CHECK_INTERVAL_MS = 60UL * 60UL * 1000UL; // 1 hour
// NVS is slot-based; camera records have silently failed to save once it
// filled. Shared by the Firmware page hint and checkNvsUsage.
static const unsigned NVS_USAGE_WARN_PERCENT = 80;
// Early warning before SD writes start failing and fall back to PSRAM;
// retention usually prevents this, but not always.
static const unsigned long SD_USAGE_CHECK_INTERVAL_MS = 60UL * 60UL * 1000UL; // 1 hour
// Higher than the NVS threshold: cards are large and a 50MB reserve is already
// kept free.
static const unsigned SD_USAGE_WARN_PERCENT = 90;
static const unsigned long WIFI_RSSI_CHECK_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 minutes
// -75dBm: still reliable on 2.4GHz but well before disconnects (~-85..-90), so
// there's time to act.
static const int WIFI_RSSI_WARN_DBM = -75;
// Internal-SRAM low-water mark (ESP.getMinFreeHeap) that triggers a one-time
// alert. mbedTLS allocates from this pool; 20KB leaves room for one TLS
// handshake. The minimum never rises within a boot, so there is no re-arm.
static const uint32_t HEAP_LOW_WARN_BYTES = 20000;
static const size_t        SNAPSHOT_MAX_BYTES       = 100000;        // internal-RAM fallback cap - see note below
static const size_t        SNAPSHOT_MAX_BYTES_PSRAM = 2000000UL;     // PSRAM buffer cap - generous; real snapshots are far smaller
static const bool          VERBOSE_SOAP_LOG         = false;         // flip true to debug one camera at a time
// Hide the routine per-call SOAP success line; faults always print.
static const bool          SUPPRESS_SOAP_SUCCESS_LOG = true;

// SNAPSHOT_MAX_BYTES only applies if a PSRAM allocation fails (fragmentation);
// PSRAM itself is mandatory (setup() halts without it).

// ============================================================
// Optional SD card (sd_store.h), off by default. Without it, or if no card
// responds at boot, snapshot history uses the PSRAM ring.
//
// *** VERIFY these against your board's pinout before flashing ***
// Nothing else uses SPI, so any four free GPIOs work.
static const int SD_CS_PIN   = 10;
static const int SD_SCK_PIN  = 12;
static const int SD_MISO_PIN = 13;
static const int SD_MOSI_PIN = 11;

// Kept free at all times; a write that would cross it prunes first.
static const uint64_t SD_FREE_SPACE_RESERVE_BYTES = 50UL * 1024UL * 1024UL; // 50MB

// Per-camera ceiling so one busy camera can't crowd out another's history
// (pruning is per camera).
static const size_t SD_MAX_FILES_PER_CAMERA = 300;

// Bounds how long one write holds the SD mutex while pruning; the next write
// continues.
static const size_t SD_PRUNE_MAX_FILES_PER_WRITE = 5;

// How long a deliberate reboot waits for in-flight SD work before proceeding.
static const unsigned long SD_IDLE_WAIT_TIMEOUT_MS = 10000UL;

static const uint32_t SD_CHECK_INTERVAL_MAX_HOURS = 720;

// Default and ceiling for snapshot retention. 30 days is a common default in
// data protection guidance for private CCTV (e.g. Portugal) - a sensible
// default, not a compliance guarantee. 0 means keep forever.
static const uint16_t SD_RETENTION_DAYS_DEFAULT = 30;
static const uint16_t SD_RETENTION_MAX_DAYS = 3650; // ~10 years

// Independent of the storage-check interval, which may be off.
static const unsigned long SD_RETENTION_CHECK_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;

// NTP resync ceiling. Clamped both at save and at use, since imported configs
// bypass the form (30 days).
static const unsigned long NTP_SYNC_MAX_MINUTES = 43200UL;

// Alert-throttling clamps, applied at save and again at use: an unclamped
// cooldown/burst pair is how multi-camera Telegram bursts broke TLS before.
static const unsigned long CAMERA_ALERT_COOLDOWN_MAX_MS = 86400000UL;    // 24h
static const unsigned long CAMERA_OFFLINE_THRESHOLD_MAX_MS = 604800000UL; // 7 days
static const unsigned int CAMERA_SNAPSHOT_BURST_MAX = 10;

// Sanity ceiling for the {WIDTH}/{HEIGHT} snapshot URI tokens.
static const uint16_t CAMERA_SNAPSHOT_DIMENSION_MAX = 4096;

// Multi-camera digest window, fixed from the first alert (a sliding window
// might never close). Only adds a summary; per-camera alerts are never
// delayed.
static const unsigned long MULTI_CAMERA_DIGEST_WINDOW_MS = 30UL * 1000UL;

static const uint16_t TELEGRAM_MAX_COMMANDS_PER_MINUTE_MAX = 600;

// SD mirror of the Activity log. Past this size the file is wiped and
// restarted, keeping it small enough to read back in one String.
static const size_t ACTIVITY_LOG_MAX_BYTES = 65536; // 64KB

// Each thumbnail triggers a directory listing, so this also bounds SD work per
// page load.
static const size_t GALLERY_PAGE_SIZE = 30;

// ============================================================
// Optional DS3231 RTC (rtc_store.h), off by default. Seeds the clock at boot,
// before WiFi/NTP can run.
//
// *** VERIFY these against your board's pinout before flashing ***
static const int RTC_SDA_PIN = 8;
static const int RTC_SCL_PIN = 9;
static const uint8_t DS3231_I2C_ADDR = 0x68; // fixed by the chip itself, not configurable

// ============================================================
// Optional internet watchdog (net_watchdog.h), off by default. Power-cycles a
// 4G router through a relay when WAN is down but local WiFi is still up.
//
// *** VERIFY this pin is actually free on your board before enabling ***
// Only a prefill; the pin is set on the Hardware page, which rejects unsafe
// pins.
static const int NET_WATCHDOG_PIN_DEFAULT = 4;

// Probe cadence; must stay well under the (configurable) outage threshold.
static const unsigned long NET_WATCHDOG_CHECK_INTERVAL_MS = 30UL * 1000UL;

// Clamps for imported configs. PULSE_MAX keeps the relay pulse, which blocks
// loop(), under the 90s task watchdog.
static const uint32_t NET_WATCHDOG_PULSE_MAX_MS = 60UL * 1000UL;      // 60s
static const uint32_t NET_WATCHDOG_THRESHOLD_MAX_MS = 60UL * 60UL * 1000UL; // 1h

// Bridge watchdog (bridge_watchdog.h): power-cycles a wireless bridge when
// both of its cameras are offline too long (one camera offline is more likely
// the camera itself).
//
// *** VERIFY this pin is actually free on your board before enabling ***
// Different default from NET_WATCHDOG_PIN_DEFAULT so both can run.
static const int BRIDGE_WATCHDOG_PIN_DEFAULT = 5;

static const unsigned long BRIDGE_WATCHDOG_CHECK_INTERVAL_MS = 30UL * 1000UL;

static const uint32_t BRIDGE_WATCHDOG_PULSE_MAX_MS = 60UL * 1000UL;      // 60s
static const uint32_t BRIDGE_WATCHDOG_THRESHOLD_MAX_MS = 60UL * 60UL * 1000UL; // 1h

// Mains power monitor (power_monitor.h): an input from a relay driven by a
// 220V-to-5V transformer; NO contact closed = power present. Alert-only.
//
// *** VERIFY this pin is actually free on your board before enabling ***
// Third distinct default so all three relay/sensor features can run together.
static const int POWER_MONITOR_PIN_DEFAULT = 6;

static const unsigned long POWER_MONITOR_CHECK_INTERVAL_MS = 10UL * 1000UL;

// Debounce: a reading must disagree with the confirmed state this long (~3
// checks) before it counts as a change.
static const unsigned long POWER_MONITOR_DEBOUNCE_MS = 30UL * 1000UL;
